#pragma once

// Small POSIX file helpers for maintenance v2 state (journal, audit, sessions).

#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {
namespace maintenance_v2 {

std::int64_t nowMs();
bool readFile(const std::string& path, std::string* out);
std::string readFileOr(const std::string& path, const std::string& fallback = std::string());
bool exists(const std::string& path);           // lstat
bool isRegularFile(const std::string& path);    // lstat: regular, not a symlink
bool isDirectory(const std::string& path);
bool isSymlink(const std::string& path);
void makeDirectories(const std::string& path, unsigned mode = 0700);  // throws
// temp file in the same directory + fsync + rename + directory fsync; throws
void writeFileAtomic(const std::string& path, const std::string& content, unsigned mode = 0600);
bool removeFile(const std::string& path);
std::vector<std::string> listDirectory(const std::string& path);  // names, no "." / ".."
std::string parentOf(const std::string& path);
std::string baseName(const std::string& path);
bool appendLineDurable(const std::string& path, const std::string& line, unsigned mode = 0600);

// Exclusive advisory lock on a lock file (flock). Unlocks on destruction.
class FileLock {
public:
    FileLock(const std::string& path, bool wait);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool locked() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace maintenance_v2
}  // namespace edge_gateway
