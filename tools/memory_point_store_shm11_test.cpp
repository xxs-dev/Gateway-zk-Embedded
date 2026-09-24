#include "edge_gateway/memory_point_store.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
void incompatibleSegmentIsNeverResized() {
    const auto name = "shm11_preserve_" + std::to_string(getpid());
    const auto path = "/" + name;
    const int fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create isolated fixture");
    struct Cleanup {
        int fd;
        std::string path;
        ~Cleanup() { close(fd); shm_unlink(path.c_str()); }
    } cleanup{fd, path};
    const std::vector<unsigned char> original(4096, 0x5a);
    require(ftruncate(fd, original.size()) == 0, "fixture length");
    require(pwrite(fd, original.data(), original.size(), 0) == static_cast<ssize_t>(original.size()),
            "fixture bytes");
    bool rejected = false;
    try {
        edge_gateway::MemoryPointStore store(name, edge_gateway::MemoryStoreOpenMode::CreateOrOpen);
    } catch (const std::exception&) { rejected = true; }
    struct stat state{};
    require(fstat(fd, &state) == 0, "stat fixture");
    std::vector<unsigned char> after(original.size());
    require(pread(fd, after.data(), after.size(), 0) == static_cast<ssize_t>(after.size()), "read fixture");
    require(state.st_size == static_cast<off_t>(original.size()) && after == original,
            "CreateOrOpen must preserve every byte and length of incompatible existing SHM");
    require(rejected, "CreateOrOpen must reject incompatible existing SHM");
}
#endif
}

int main() {
    try {
#ifndef _WIN32
        incompatibleSegmentIsNeverResized();
#endif
        std::cout << "memory_point_store_shm11_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "memory_point_store_shm11_test failed: " << error.what() << '\n';
        return 1;
    }
}
