#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  pointctl write --index <index> --value <value> [--cmd <cmdId>] [--source <source>] [--shm <name>|--app-config <path>]\n"
        << "  pointctl get --index <index> [--shm <name>|--app-config <path>]\n"
        << "  pointctl dump [--limit <n>] [--owners] [--shm <name>|--app-config <path>]\n"
        << "  pointctl pending [--limit <n>] [--shm <name>]\n"
        << "  pointctl pending-peek [--limit <n>] [--shm <name>|--app-config <path>]\n";
}

std::string getOption(
    const std::vector<std::string>& args,
    const std::string& name,
    const std::string& defaultValue = ""
) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return defaultValue;
}

bool hasOption(const std::vector<std::string>& args, const std::string& name) {
    for (const auto& arg : args) {
        if (arg == name) {
            return true;
        }
    }
    return false;
}

struct RouterContext {
    edge_gateway::AppConfig appConfig;
    std::vector<edge_gateway::DeviceConfig> deviceConfigs;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    edge_gateway::PointStoreRouter router;
};

std::unique_ptr<RouterContext> createRouterContext(const std::string& appConfigPath) {
    using namespace edge_gateway;
    std::unique_ptr<RouterContext> context(new RouterContext());
    context->appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    context->deviceConfigs = ConfigLoader::loadMany(context->appConfig.deviceConfigFiles);

    std::vector<std::string> sharedMemoryNames = context->appConfig.mqttDriver.sharedMemoryNames;
    if (sharedMemoryNames.empty()) {
        sharedMemoryNames.push_back(context->appConfig.mqttDriver.sharedMemoryName);
    }
    std::unordered_set<std::string> seen(sharedMemoryNames.begin(), sharedMemoryNames.end());
    for (const auto& config : context->deviceConfigs) {
        const auto& name = config.memoryStore.sharedMemoryName;
        if (!name.empty() && seen.insert(name).second) {
            sharedMemoryNames.push_back(name);
        }
    }
    context->stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        context->stores.emplace_back(new MemoryPointStore(name));
        context->router.addStore(name, *context->stores.back());
    }
    context->router.addRoutesFromDeviceConfigs(
        context->deviceConfigs,
        context->appConfig.mqttDriver.sharedMemoryName
    );
    return context;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::vector<std::string> args(argv + 1, argv + argc);
    const auto& command = args.front();
    const auto shmName = getOption(args, "--shm", "gateway_point_store");
    const auto appConfigPath = getOption(args, "--app-config");

    try {
        if (!appConfigPath.empty()) {
            auto context = createRouterContext(appConfigPath);

            if (command == "write") {
                const auto indexText = getOption(args, "--index");
                const auto valueText = getOption(args, "--value");
                if (indexText.empty() || valueText.empty()) {
                    printUsage();
                    return 1;
                }

                PendingWriteCommand cmd;
                cmd.cmdId = getOption(args, "--cmd", "POINTCTL_" + std::to_string(nowMs()));
                cmd.index = static_cast<std::uint32_t>(std::stoul(indexText));
                cmd.value = std::stod(valueText);
                cmd.source = getOption(args, "--source", "pointctl");
                cmd.ts = nowMs();
                const auto result = context->router.submitWriteCommand(cmd);
                if (!result.accepted) {
                    std::cout << "write rejected"
                              << " cmdId=" << cmd.cmdId
                              << " index=" << cmd.index
                              << " message=" << result.message
                              << std::endl;
                    return 2;
                }

                std::cout << "submitted write"
                          << " cmdId=" << cmd.cmdId
                          << " index=" << cmd.index
                          << " value=" << cmd.value
                          << " interface=" << result.route.interfaceCode
                          << " shm=" << result.route.sharedMemoryName
                          << std::endl;
                return 0;
            }

            if (command == "get") {
                const auto indexText = getOption(args, "--index");
                if (indexText.empty()) {
                    printUsage();
                    return 1;
                }
                const auto index = static_cast<std::uint32_t>(std::stoul(indexText));
                const auto latest = context->router.getLatestByIndex(index, nowMs());
                if (!latest) {
                    std::cout << "not found" << std::endl;
                    return 2;
                }
                const auto route = context->router.routeByIndex(index);
                std::cout << "index=" << latest->index
                          << " value=" << latest->value
                          << " quality=" << latest->quality
                          << " ts=" << latest->ts
                          << " stale=" << latest->stale;
                if (route) {
                    std::cout << " interface=" << route->interfaceCode
                              << " shm=" << route->sharedMemoryName;
                }
                std::cout << std::endl;
                return 0;
            }

            if (command == "dump" || command == "snapshot") {
                const auto limitText = getOption(args, "--limit", "0");
                const auto limit = static_cast<std::size_t>(std::stoul(limitText));
                const auto ts = nowMs();
                const auto values = context->router.getAllLatest(ts);
                std::size_t count = 0;
                for (const auto& item : values) {
                    if (limit > 0 && count >= limit) {
                        break;
                    }
                    const auto route = context->router.routeByIndex(item.index);
                    std::cout << "index=" << item.index
                              << " value=" << item.value
                              << " quality=" << item.quality
                              << " ts=" << item.ts
                              << " stale=" << item.stale;
                    if (route) {
                        std::cout << " interface=" << route->interfaceCode
                                  << " shm=" << route->sharedMemoryName;
                    }
                    std::cout << std::endl;
                    ++count;
                }
                std::cout << "latest_count=" << values.size() << std::endl;
                return 0;
            }

            if (command == "pending-peek") {
                const auto limitText = getOption(args, "--limit", "0");
                const auto limit = static_cast<std::size_t>(std::stoul(limitText));
                const auto pending = context->router.peekPendingWrites(limit);
                std::cout << "pending_count=" << pending.size() << std::endl;
                for (const auto& item : pending) {
                    const auto route = context->router.routeByIndex(item.index);
                    std::cout << "cmdId=" << item.cmdId
                              << " index=" << item.index
                              << " value=" << item.value
                              << " source=" << item.source
                              << " ts=" << item.ts;
                    if (route) {
                        std::cout << " interface=" << route->interfaceCode
                                  << " shm=" << route->sharedMemoryName;
                    }
                    std::cout << std::endl;
                }
                return 0;
            }

            printUsage();
            return 1;
        }

        MemoryPointStore store(shmName);

        if (command == "write") {
            const auto indexText = getOption(args, "--index");
            const auto valueText = getOption(args, "--value");
            if (indexText.empty() || valueText.empty()) {
                printUsage();
                return 1;
            }

            PendingWriteCommand cmd;
            cmd.cmdId = getOption(args, "--cmd", "POINTCTL_" + std::to_string(nowMs()));
            cmd.index = static_cast<std::uint32_t>(std::stoul(indexText));
            cmd.value = std::stod(valueText);
            cmd.source = getOption(args, "--source", "pointctl");
            cmd.ts = nowMs();
            store.submitWriteCommand(cmd);

            std::cout << "submitted write"
                      << " cmdId=" << cmd.cmdId
                      << " index=" << cmd.index
                      << " value=" << cmd.value
                      << " shm=" << shmName
                      << std::endl;
            return 0;
        }

        if (command == "get") {
            const auto indexText = getOption(args, "--index");
            if (indexText.empty()) {
                printUsage();
                return 1;
            }

            const auto latest = store.getLatestByIndex(
                static_cast<std::uint32_t>(std::stoul(indexText)),
                nowMs()
            );
            if (!latest) {
                std::cout << "not found" << std::endl;
                return 2;
            }

            std::cout << "index=" << latest->index
                      << " value=" << latest->value
                      << " quality=" << latest->quality
                      << " ts=" << latest->ts
                      << " stale=" << latest->stale
                      << std::endl;
            return 0;
        }

        if (command == "pending") {
            const auto limitText = getOption(args, "--limit", "0");
            const auto limit = static_cast<std::size_t>(std::stoul(limitText));
            const auto pending = store.drainPendingWriteCommands(limit);
            std::cout << "pending_count=" << pending.size() << std::endl;
            for (const auto& item : pending) {
                std::cout << "cmdId=" << item.cmdId
                          << " index=" << item.index
                          << " value=" << item.value
                          << " source=" << item.source
                          << " ts=" << item.ts
                          << std::endl;
            }
            return 0;
        }

        if (command == "pending-peek") {
            const auto limitText = getOption(args, "--limit", "0");
            const auto limit = static_cast<std::size_t>(std::stoul(limitText));
            const auto pending = store.peekPendingWriteCommands(limit);
            std::cout << "pending_count=" << pending.size() << std::endl;
            for (const auto& item : pending) {
                std::cout << "cmdId=" << item.cmdId
                          << " index=" << item.index
                          << " value=" << item.value
                          << " source=" << item.source
                          << " ts=" << item.ts
                          << std::endl;
            }
            return 0;
        }

        if (command == "dump" || command == "snapshot") {
            const auto limitText = getOption(args, "--limit", "0");
            const auto limit = static_cast<std::size_t>(std::stoul(limitText));
            const auto ts = nowMs();
            const auto values = store.getAllLatest(ts);
            const auto showOwners = hasOption(args, "--owners");
            std::unordered_map<std::uint32_t, PointLeaseStatus> leaseByIndex;
            if (showOwners) {
                for (const auto& item : store.getAllLeaseStatus(ts)) {
                    leaseByIndex[item.index] = item;
                }
            }
            std::size_t count = 0;
            for (const auto& item : values) {
                if (limit > 0 && count >= limit) {
                    break;
                }
                std::cout << "index=" << item.index
                          << " value=" << item.value
                          << " quality=" << item.quality
                          << " ts=" << item.ts
                          << " stale=" << item.stale;
                if (showOwners) {
                    const auto it = leaseByIndex.find(item.index);
                    if (it != leaseByIndex.end()) {
                        std::cout << " owners=" << it->second.ownerCount
                                  << " active=" << it->second.hasActiveOwner
                                  << " lastClaimTs=" << it->second.lastClaimTs;
                    } else {
                        std::cout << " owners=0 active=0 lastClaimTs=0";
                    }
                }
                std::cout << std::endl;
                ++count;
            }
            std::cout << "latest_count=" << values.size() << std::endl;
            return 0;
        }

        printUsage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "pointctl error: " << ex.what() << std::endl;
        return 1;
    }
}
