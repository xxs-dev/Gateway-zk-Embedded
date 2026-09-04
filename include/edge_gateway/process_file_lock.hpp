#pragma once

#include <string>

namespace edge_gateway {

class ProcessFileLock {
public:
    explicit ProcessFileLock(std::string path);
    ~ProcessFileLock();

    ProcessFileLock(const ProcessFileLock&) = delete;
    ProcessFileLock& operator=(const ProcessFileLock&) = delete;

    bool tryAcquire();

private:
    std::string path_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace edge_gateway
