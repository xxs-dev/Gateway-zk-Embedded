#define main unusedCoordinatorEntrypoint
#ifdef EMS_MAIN_SOURCE
#include EMS_MAIN_SOURCE
#else
#include "../ems_cluster_main.cpp"
#endif
#undef main

#include <ctime>
#include <iomanip>
#include <vector>

namespace {
constexpr std::int64_t fixedNow = 1000000;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct TempHealth {
    std::string path;
    TempHealth() {
        char name[] = "/tmp/ems-health-XXXXXX";
        int fd = mkstemp(name);
        require(fd >= 0, "create private health file");
        close(fd);
        path = name;
    }
    ~TempHealth() { std::remove(path.c_str()); }
    void put(const std::string& text) {
        std::ofstream stream(path);
        stream << text;
        require(static_cast<bool>(stream), "write health fixture");
    }
};
double cpuUs() {
    timespec ts{};
    require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0, "CPU clock");
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
std::string health(std::int64_t ts, const std::string& suffix = "") {
    return "{\"ts\":" + std::to_string(ts) +
        ",\"healthy\":true,\"timeoutPercent\":0,\"controlQueueP95Ms\":2" + suffix + "}";
}
}

// Freeze only this translation unit's wall-clock reads, preserving /proc sampling.
extern "C" std::chrono::system_clock::time_point frozenSystemNow()
    asm("__wrap__ZNSt6chrono3_V212system_clock3nowEv");
extern "C" std::chrono::system_clock::time_point frozenSystemNow() {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(fixedNow));
}

int main() {
    try {
        require(wallNowMs() == fixedNow, "wall clock wrapper did not bind");
        TempHealth file;
        LoadSampler sampler(file.path);
        const std::vector<std::string> cases{
            "", "{}", "not json", health(fixedNow), health(fixedNow - 5000),
            health(fixedNow - 5001), health(fixedNow + 60000), health(fixedNow + 60001),
            health(0), health(-1),
            R"({"ts":1000000,"healthy":false,"timeoutPercent":-2.5,"controlQueueP95Ms":-0.25})",
            R"({"ts":1000000,"healthy":"true","timeoutPercent":"2","controlQueueP95Ms":null})",
            R"({"ts":1000000,"healthy":TRUE,"timeoutPercent":NaN,"controlQueueP95Ms":Infinity})",
            R"({"ts":1000000,"healthy":true,"timeoutPercent":1e3,"controlQueueP95Ms":.5})",
            R"({"ts":1000000.75,"healthy":true,"timeoutPercent":+2,"controlQueueP95Ms":02})",
            R"({"ts":1000000,"ts":0,"healthy":false,"healthy":true,"timeoutPercent":3,"timeoutPercent":4})",
            R"({"nested":{"ts":1000000,"healthy":true},"timeoutPercent":-1.2.3})",
            "{\n\"ts\" \t: 1000000,\n\"healthy\" : true,\"timeoutPercent\":2.75,\"controlQueueP95Ms\":3.5}\r\n",
            health(fixedNow, ",\"other\":\"text\""),
            "{\"ts\":" + std::string(400, '9') + "}",
            "{\"ts\":1000000,\"timeoutPercent\":" + std::string(400, '9') + "}",
            "{\"ts\":1000000,\"controlQueueP95Ms\":" + std::string(400, '9') + "}"
        };
        for (std::size_t i = 0; i < cases.size(); ++i) {
            file.put(cases[i]);
            try {
                const auto result = sampler.sample();
                if (i == 3 || i == 4 || i == 6) require(result.computeHealthy, "freshness inclusive boundary");
                if (i == 0 || i == 5 || i == 7 || i == 8 || i == 9)
                    require(!result.computeMetricsAvailable && !result.computeHealthy, "unavailable health gate");
                require(i < 19, "out-of-range conversion must still throw");
                std::cout << std::setprecision(17) << "CASE " << i << ' ' << result.computeMetricsAvailable
                    << ' ' << result.computeHealthy << ' ' << result.computeTimeoutPercent
                    << ' ' << result.controlQueueP95Ms << ' ' << result.packetLossPercent << '\n';
            } catch (const std::out_of_range&) {
                require(i >= 19, "unexpected out-of-range exception");
                std::cout << "CASE " << i << " out_of_range\n";
            }
        }
        std::remove(file.path.c_str());
        const auto missing = sampler.sample();
        require(!missing.computeMetricsAvailable && !missing.computeHealthy &&
            missing.computeTimeoutPercent == 100 && missing.controlQueueP95Ms == 1000, "missing health file defaults");
        std::cout << "CASE missing_file 0 0 100 1000\n";
        for (int fresh = 0; fresh < 2; ++fresh) {
            file.put(fresh ? health(fixedNow) : "");
            for (int i = 0; i < 5; ++i) sampler.sample();
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto begin = cpuUs();
                for (int i = 0; i < 200; ++i) {
                    const auto result = sampler.sample();
                    require(result.computeHealthy == (fresh != 0), "metrics reuse retained prior state");
                }
                std::cout << std::setprecision(12) << "BENCH {\"fresh\":" << fresh
                    << ",\"repeat\":" << repeat << ",\"iterations\":200,\"cpuUsPerCall\":"
                    << (cpuUs() - begin) / 200 << "}\n";
            }
        }
        std::cout << "PASS load sampler\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
