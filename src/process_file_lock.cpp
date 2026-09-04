#include "edge_gateway/process_file_lock.hpp"

#include <cerrno>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace edge_gateway {

ProcessFileLock::ProcessFileLock(std::string path) : path_(std::move(path)) {
}

ProcessFileLock::~ProcessFileLock() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        close(fd_);
    }
#endif
}

bool ProcessFileLock::tryAcquire() {
    if (path_.empty()) {
        return true;
    }
#ifdef _WIN32
    const auto handle = CreateFileA(
        path_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    handle_ = handle;
    return true;
#else
    fd_ = open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("failed to open process lock: " + path_);
    }
    if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        return true;
    }
    const auto error = errno;
    close(fd_);
    fd_ = -1;
    if (error == EWOULDBLOCK || error == EAGAIN) {
        return false;
    }
    throw std::runtime_error("failed to acquire process lock: " + path_);
#endif
}

}  // namespace edge_gateway
