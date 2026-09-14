#pragma once

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

// Only call for a test-owned mkdtemp directory. Never follow symlinks during cleanup.
inline void removeEventStoreTestTree(const std::string& path) noexcept {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { unlink(path.c_str()); return; }
    auto* directory = opendir(path.c_str());
    if (!directory) return;
    while (auto* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") removeEventStoreTestTree(path + "/" + name);
    }
    closedir(directory);
    rmdir(path.c_str());
}
