#pragma once
#include <fstream>
#include <sstream>
#include <string>

namespace edge_gateway {
// Linux boot ID plus process start ticks distinguish stale files and PID reuse.
inline std::string computeProcessIdentity(long long pid) {
#ifdef __linux__
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::string bootId;
    std::getline(boot, bootId);
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const auto end = line.rfind(')');
    if (bootId.size() != 36 || end == std::string::npos) return "";
    std::istringstream fields(line.substr(end + 1));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) return "";
        if (field == 3 && (value == "Z" || value == "X")) return "";
    }
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) return "";
    return bootId + ":" + std::to_string(pid) + ":" + value;
#else
    (void)pid;
    return "";
#endif
}
} // namespace edge_gateway
