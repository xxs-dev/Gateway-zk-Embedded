#include "../src/memory_point_store_migration.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <vector>

namespace {
void report(const std::string& segment, const edge_gateway::OfflineMigrationResult& result, bool check) {
    std::cout << (check ? "CHECK" : "MIGRATED") << " shm=" << segment << " version=" << result.oldVersion
              << " target=10 occupied_before=" << result.occupiedBefore
              << " occupied_after=" << result.occupiedAfter << " duplicate_groups=" << result.duplicateGroups.size()
              << " removed_count=" << result.removedCount << '\n';
    for (const auto& group : result.duplicateGroups) {
        std::cout << "index=" << group.index << " winner_slot=" << group.winnerSlot << " removed_slots=";
        for (std::size_t i = 0; i < group.removedSlots.size(); ++i)
            std::cout << (i ? "," : "") << group.removedSlots[i];
        std::cout << '\n';
    }
}
}

int main(int argc, char** argv) {
    try {
        std::string backup;
        std::string target;
        std::vector<std::string> segments;
        edge_gateway::OfflineMigrationOptions options;
        bool offline = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--offline-confirmed" && !offline) offline = true;
            else if (arg == "--check" && !options.checkOnly) options.checkOnly = true;
            else if (arg == "--deduplicate-latest" && !options.deduplicateLatest) options.deduplicateLatest = true;
            else if (arg == "--shm" && i + 1 < argc) {
                std::string segment = argv[++i];
                if (!segment.empty() && segment.front() == '/') segment.erase(0, 1);
                if (std::find(segments.begin(), segments.end(), segment) != segments.end())
                    throw std::runtime_error("repeated segment");
                segments.push_back(segment);
            }
            else if (arg == "--backup" && backup.empty() && i + 1 < argc) backup = argv[++i];
            else if (arg == "--copy-to-v11" && target.empty() && i + 1 < argc) target = argv[++i];
            else throw std::runtime_error("unknown, repeated or incomplete argument: " + arg);
        }
        if (segments.empty() || !offline ||
            (options.checkOnly ? !backup.empty() : (backup.empty() || segments.size() != 1)))
            throw std::runtime_error("Usage: memory_point_store_migrate --shm NAME --offline-confirmed "
                "[--deduplicate-latest] (--backup /persistent/unique.bak | --check [--shm NAME ...])");
        bool failed = false;
        if (!target.empty()) {
            if (segments.size() != 1 || options.checkOnly || options.deduplicateLatest)
                throw std::runtime_error("--copy-to-v11 requires one source, a backup and no in-place options");
            const auto copied = edge_gateway::copyOfflinePointStoreV10ToV11(segments.front(), target, backup, offline);
            std::cout << "COPIED source=" << segments.front() << " version=" << copied.oldVersion
                      << " target=" << target << " target_version=11 source_unchanged=true\n";
            return 0;
        }
        for (const auto& segment : segments) {
            try {
                report(segment, edge_gateway::migrateOfflinePointStore(segment, backup, offline, options), options.checkOnly);
            } catch (const std::exception& error) {
                if (!options.checkOnly) throw;
                failed = true;
                std::cerr << "CHECK FAILED shm=" << segment << ": " << error.what() << '\n';
            }
        }
        if (!options.checkOnly) std::cout << "Verified allowed offsets only. Backup: " << backup << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Migration refused/failed: " << error.what()
                  << "\nKeep all participants stopped. No segment has been unlinked.\n";
        return 1;
    }
}
