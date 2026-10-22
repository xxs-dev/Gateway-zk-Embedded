#include "v2_files.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) return false;
    *out = buffer.str();
    return true;
}

std::string readFileOr(const std::string& path, const std::string& fallback) {
    std::string text;
    return readFile(path, &text) ? text : fallback;
}

#ifndef _WIN32

bool exists(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

bool isRegularFile(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isSymlink(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

void makeDirectories(const std::string& path, unsigned mode) {
    if (path.empty()) return;
    std::string current;
    std::size_t cursor = 0;
    if (path[0] == '/') {
        current = "/";
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const auto next = path.find('/', cursor);
        const auto part = path.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current += '/';
            current += part;
            if (::mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                throw std::runtime_error("cannot create directory " + current + ": " + std::strerror(errno));
            }
        }
        if (next == std::string::npos) break;
        cursor = next + 1;
    }
}

namespace {

void syncDirectory(const std::string& dir) {
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

bool writeAllFd(int fd, const std::string& content) {
    std::size_t written = 0;
    while (written < content.size()) {
        const auto rc = ::write(fd, content.data() + written, content.size() - written);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<std::size_t>(rc);
    }
    return true;
}

}  // namespace

void writeFileAtomic(const std::string& path, const std::string& content, unsigned mode) {
    const auto dir = parentOf(path);
    thread_local std::mt19937_64 random(std::random_device{}());
    const auto temp = path + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(random());
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) throw std::runtime_error("cannot write " + temp + ": " + std::strerror(errno));
    const bool ok = writeAllFd(fd, content) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) {
        const std::string reason = std::strerror(errno);
        ::unlink(temp.c_str());
        throw std::runtime_error("cannot write " + path + ": " + reason);
    }
    syncDirectory(dir);
}

bool removeFile(const std::string& path) {
    if (::unlink(path.c_str()) != 0) return errno == ENOENT;
    syncDirectory(parentOf(path));
    return true;
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> names;
    DIR* handle = ::opendir(path.c_str());
    if (handle == nullptr) return names;
    while (auto* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(handle);
    return names;
}

bool appendLineDurable(const std::string& path, const std::string& line, unsigned mode) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const bool ok = writeAllFd(fd, line + "\n") && ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

FileLock::FileLock(const std::string& path, bool wait) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;
    while (::flock(fd_, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
        if (errno == EINTR) continue;
        ::close(fd_);
        fd_ = -1;
        return;
    }
}

FileLock::~FileLock() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

#else
bool exists(const std::string&) { return false; }
bool isRegularFile(const std::string&) { return false; }
bool isDirectory(const std::string&) { return false; }
bool isSymlink(const std::string&) { return false; }
void makeDirectories(const std::string&, unsigned) {}
void writeFileAtomic(const std::string&, const std::string&, unsigned) { throw std::runtime_error("unsupported"); }
bool removeFile(const std::string&) { return false; }
std::vector<std::string> listDirectory(const std::string&) { return {}; }
bool appendLineDurable(const std::string&, const std::string&, unsigned) { return false; }
FileLock::FileLock(const std::string&, bool) {}
FileLock::~FileLock() {}
#endif

std::string parentOf(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    return pos == 0 ? "/" : path.substr(0, pos);
}

std::string baseName(const std::string& path) {
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
