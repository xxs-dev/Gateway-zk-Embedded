#include "../src/memory_point_store_migration.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        std::string segment, backup;
        bool offline = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--offline-confirmed" && !offline) offline = true;
            else if (arg == "--shm" && segment.empty() && i + 1 < argc) segment = argv[++i];
            else if (arg == "--backup" && backup.empty() && i + 1 < argc) backup = argv[++i];
            else throw std::runtime_error("unknown, repeated or incomplete argument: " + arg);
        }
        if (segment.empty() || backup.empty() || !offline)
            throw std::runtime_error("Usage: memory_point_store_migrate --shm NAME --backup /persistent/unique.bak --offline-confirmed");
        const auto oldVersion = edge_gateway::migrateOfflinePointStore(segment, backup, offline);
        std::cout << "Verified SHM v" << oldVersion << " -> v10; only version changed. Backup: " << backup << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Migration refused/failed: " << error.what()
                  << "\nKeep all participants stopped. No segment has been unlinked.\n";
        return 1;
    }
}
