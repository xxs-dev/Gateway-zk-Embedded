#pragma once

#include "edge_gateway/event_store.hpp"

#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <time.h>
#include <unistd.h>
#endif

namespace edge_gateway {

inline EventStoreLeaseTime readEventStoreLeaseTime() {
#if !defined(__linux__)
    throw std::runtime_error("EventStoreLeaseTime is unsupported on this platform");
#else
    static const std::string bootId = [] {
        const int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("failed to open boot_id");
        char buffer[64];
        ssize_t count;
        do { count = ::read(fd, buffer, sizeof(buffer)); } while (count < 0 && errno == EINTR);
        ::close(fd);
        if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(buffer)) {
            throw std::runtime_error("failed bounded boot_id read");
        }
        auto size = static_cast<std::size_t>(count);
        if (buffer[size - 1] == '\n') --size;
        if (size != 36) throw std::runtime_error("boot_id is not a canonical UUID");
        for (std::size_t i = 0; i < size; ++i) {
            const char c = buffer[i];
            const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
            if (hyphen ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                throw std::runtime_error("boot_id is not a canonical UUID");
            }
        }
        return std::string(buffer, size);
    }();
    timespec time{};
    if (::clock_gettime(CLOCK_BOOTTIME, &time) != 0) throw std::runtime_error("CLOCK_BOOTTIME unavailable");
    if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000L) {
        throw std::runtime_error("invalid CLOCK_BOOTTIME timespec");
    }
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (static_cast<std::uint64_t>(time.tv_sec) > static_cast<std::uint64_t>(maximum / 1000)) {
        throw std::runtime_error("CLOCK_BOOTTIME millisecond overflow");
    }
    const auto base = static_cast<std::int64_t>(time.tv_sec) * 1000;
    const auto fraction = static_cast<std::int64_t>(time.tv_nsec / 1000000L);
    if (base > maximum - fraction) throw std::runtime_error("CLOCK_BOOTTIME millisecond overflow");
    return {bootId, base + fraction};
#endif
}

} // namespace edge_gateway
