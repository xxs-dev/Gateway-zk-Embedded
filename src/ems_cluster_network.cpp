#include "edge_gateway/ems_cluster_network.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) ++start;
    return value.substr(start);
}

std::string readFirstLine(const std::string& path) {
    std::ifstream input(path.c_str());
    std::string value;
    std::getline(input, value);
    return trim(value);
}

bool validInterfaceName(const std::string& value) {
    if (value.empty() || value.size() > 15) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

bool containsAddress(const EmsClusterInterfaceInfo& info, const std::string& address) {
    return std::find(info.ipv4Addresses.begin(), info.ipv4Addresses.end(), address) != info.ipv4Addresses.end();
}

bool isLinkLocal(const std::string& address) {
    return address.rfind("169.254.", 0) == 0;
}

#ifndef _WIN32
int runProgram(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return -1;
    const auto pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    for (const auto ch : path) {
        current.push_back(ch);
        if (ch == '/' && current.size() > 1) mkdir(current.c_str(), 0775);
    }
    mkdir(path.c_str(), 0775);
}

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void persistAddressState(
    const EmsClusterConfig& config,
    const std::string& machineCode,
    const std::string& macAddress,
    const std::string& address
) {
    if (config.networkStateFile.empty()) return;
    ensureDirectory(directoryOf(config.networkStateFile));
    const auto temporary = config.networkStateFile + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    std::ofstream output(temporary.c_str(), std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to write EMS cluster network state");
    output << "{\n"
           << "  \"schemaVersion\": \"1.0\",\n"
           << "  \"machineCode\": \"" << machineCode << "\",\n"
           << "  \"interface\": \"" << config.clusterInterface << "\",\n"
           << "  \"macAddress\": \"" << macAddress << "\",\n"
           << "  \"address\": \"" << address << "\",\n"
           << "  \"prefixLength\": " << config.prefixLength << "\n"
           << "}\n";
    output.close();
    if (!output || std::rename(temporary.c_str(), config.networkStateFile.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("failed to install EMS cluster network state");
    }
}
#endif

}  // namespace

EmsClusterInterfaceInfo inspectEmsClusterInterface(const std::string& interfaceName) {
    EmsClusterInterfaceInfo result;
    result.name = interfaceName;
    if (!validInterfaceName(interfaceName)) return result;
#ifndef _WIN32
    result.exists = if_nametoindex(interfaceName.c_str()) != 0;
    if (!result.exists) return result;
    const auto sysfs = std::string("/sys/class/net/") + interfaceName;
    struct stat info{};
    result.physical = stat((sysfs + "/device").c_str(), &info) == 0;
    result.macAddress = readFirstLine(sysfs + "/address");
    const auto operState = readFirstLine(sysfs + "/operstate");
    result.linkUp = operState == "up" || operState == "unknown";

    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
        for (auto* item = addresses; item != nullptr; item = item->ifa_next) {
            if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
                interfaceName != item->ifa_name) continue;
            char buffer[INET_ADDRSTRLEN]{};
            const auto* address = &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, address, buffer, sizeof(buffer)) != nullptr) {
                result.ipv4Addresses.emplace_back(buffer);
            }
        }
        freeifaddrs(addresses);
    }
#endif
    return result;
}

std::string deriveEmsClusterLinkLocalAddress(
    const std::string& machineCode,
    const std::string& macAddress,
    unsigned int salt
) {
    const auto hash = fnv1a64(machineCode + "|" + macAddress + "|" + std::to_string(salt));
    const auto third = 1U + static_cast<unsigned int>((hash >> 8U) % 254U);
    const auto fourth = 1U + static_cast<unsigned int>(hash % 254U);
    return "169.254." + std::to_string(third) + "." + std::to_string(fourth);
}

EmsClusterNetworkCheck checkEmsClusterNetwork(
    const EmsClusterConfig& config,
    const std::string& machineCode
) {
    EmsClusterNetworkCheck result;
    const auto info = inspectEmsClusterInterface(config.clusterInterface);
    if (!info.exists) result.errors.push_back("并机网卡不存在: " + config.clusterInterface);
    if (info.exists && !info.physical && config.clusterInterface != "lo") {
        result.errors.push_back("并机网卡不是实体网卡: " + config.clusterInterface);
    }
    if (info.exists && containsAddress(info, config.factoryAddress)) {
        result.errors.push_back("并机网卡仍使用出厂固定地址 " + config.factoryAddress + "，拒绝启动以避免重复 IP");
    }
    if (!info.linkUp) result.warnings.push_back("并机网卡当前没有物理链路");
    if (machineCode.empty()) result.errors.push_back("machineCode 为空，无法生成稳定 Link-Local 地址");

    if (config.ipMode == "autoLinkLocal") {
        result.candidateAddress = deriveEmsClusterLinkLocalAddress(machineCode, info.macAddress);
        const auto current = std::find_if(info.ipv4Addresses.begin(), info.ipv4Addresses.end(), isLinkLocal);
        if (current != info.ipv4Addresses.end()) result.selectedAddress = *current;
        if (result.selectedAddress.empty()) {
            result.errors.push_back(
                "并机网卡尚未配置 Link-Local 地址；候选地址为 " + result.candidateAddress +
                "，需先执行显式网络应用和 ARP DAD"
            );
        }
    } else if (config.ipMode == "static") {
        result.candidateAddress = config.staticAddress;
        if (config.staticAddress.empty()) {
            result.errors.push_back("静态并机模式缺少 staticAddress");
        } else if (containsAddress(info, config.staticAddress)) {
            result.selectedAddress = config.staticAddress;
        } else {
            result.errors.push_back("并机网卡未配置指定静态地址 " + config.staticAddress);
        }
    } else {
        result.errors.push_back("不支持的并机地址模式: " + config.ipMode);
    }
    result.ready = result.errors.empty();
    return result;
}

EmsClusterNetworkCheck prepareEmsClusterNetwork(
    const EmsClusterConfig& config,
    const std::string& machineCode
) {
#ifdef _WIN32
    EmsClusterNetworkCheck result;
    result.errors.push_back("Windows 不支持应用边端并机网卡配置");
    return result;
#else
    const auto info = inspectEmsClusterInterface(config.clusterInterface);
    EmsClusterNetworkCheck result;
    if (!info.exists || (!info.physical && config.clusterInterface != "lo")) {
        result.errors.push_back("并机网卡不存在或不是实体网卡: " + config.clusterInterface);
        return result;
    }
    auto address = config.ipMode == "static" ? config.staticAddress : std::string();
    if (config.ipMode == "autoLinkLocal") {
        address = deriveEmsClusterLinkLocalAddress(machineCode, info.macAddress);
    }
    result.candidateAddress = address;
    if (address.empty()) {
        result.errors.push_back("无法确定并机地址");
        return result;
    }
    if (containsAddress(info, address) && !containsAddress(info, config.factoryAddress)) {
        result.selectedAddress = address;
        result.ready = true;
        return result;
    }
    if (runProgram({"ip", "link", "set", "dev", config.clusterInterface, "up"}) != 0) {
        result.errors.push_back("无法启用并机网卡");
        return result;
    }
    if (config.ipMode == "autoLinkLocal") {
        bool available = false;
        for (unsigned int salt = 0; salt < 16; ++salt) {
            address = deriveEmsClusterLinkLocalAddress(machineCode, info.macAddress, salt);
            result.candidateAddress = address;
            const auto dad = runProgram({
                "arping", "-D", "-I", config.clusterInterface, "-c", "3", "-w", "4", address
            });
            if (dad == 127) {
                result.errors.push_back("缺少 arping，无法完成 Link-Local 重复地址检测");
                return result;
            }
            if (dad == 0) {
                available = true;
                break;
            }
        }
        if (!available) {
            result.errors.push_back("16 个稳定 Link-Local 候选地址均发生冲突");
            return result;
        }
    }
    if (containsAddress(info, config.factoryAddress)) {
        runProgram({"ip", "address", "del", config.factoryAddress + "/24", "dev", config.clusterInterface});
    }
    if (runProgram({
        "ip", "address", "replace", address + "/" + std::to_string(config.prefixLength),
        "dev", config.clusterInterface
    }) != 0) {
        result.errors.push_back("无法应用并机地址 " + address);
        return result;
    }
    persistAddressState(config, machineCode, info.macAddress, address);
    result.selectedAddress = address;
    result.ready = true;
    result.warnings.push_back("已显式应用并机地址；未添加默认路由和 DNS");
    return result;
#endif
}

}  // namespace edge_gateway
