#include "edge_gateway/channel_trace.hpp"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace {
constexpr std::uint32_t magic = 0x47575431;
constexpr std::size_t capacity = 512, maxBytes = 2048;
struct PreserveErrno { int saved = errno; ~PreserveErrno() { errno = saved; } };
std::int64_t wall() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t mono() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Slot { std::uint64_t seq; std::int64_t ts; std::uint32_t length, originalLength; bool tx; std::uint8_t bytes[maxBytes]; };
struct Ring { std::uint32_t version; char session[65], channel[1024]; std::int64_t deadline, expiresAt; std::uint64_t seq; Slot slots[capacity]; };
#ifndef _WIN32
struct Mapping {
    int fd = -1; Ring* ring = nullptr;
    Mapping(const std::string& path, bool create) {
        fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
        if (fd < 0) throw std::runtime_error("trace buffer unavailable");
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1 || flock(fd, LOCK_EX | LOCK_NB)) { close(fd); fd = -1; throw std::runtime_error("trace buffer unsafe or busy; try later"); }
        if (st.st_size != sizeof(Ring)) {
            if (!create || (st.st_size != 0) || ftruncate(fd, sizeof(Ring))) { close(fd); fd = -1; throw std::runtime_error("trace buffer version/size mismatch"); }
        }
        auto ptr = mmap(nullptr, sizeof(Ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { close(fd); fd = -1; throw std::runtime_error("trace mmap failed"); }
        ring = static_cast<Ring*>(ptr);
    }
    ~Mapping() { if (ring) munmap(ring, sizeof(Ring)); if (fd >= 0) close(fd); }
};
void check(const Ring& ring, const std::string& session) {
    if (ring.version != magic || session.empty() || session.size() > 64 || session != ring.session) throw std::runtime_error("trace session mismatch or expired buffer");
}
#endif
std::string processChannel;
std::mutex processMutex;
}
ChannelTrace::ChannelTrace(std::string path) : path_(std::move(path)) {}
void ChannelTrace::start(const std::string& channel, const std::string& session, int seconds) {
    if (channel.empty() || channel.size() > 1023 || channel.find('\0') != std::string::npos || session.empty() || session.size() > 64 || !std::all_of(session.begin(), session.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }) || seconds < 1 || seconds > 300) throw std::runtime_error("invalid trace channel/session/duration (1-300s)");
#ifndef _WIN32
    Mapping m(path_, true); auto& r = *m.ring;
    if (r.version == magic && r.deadline > mono()) throw std::runtime_error("a trace session is already active");
    std::memset(&r, 0, sizeof(r));
    std::memcpy(r.channel, channel.c_str(), channel.size()); std::memcpy(r.session, session.c_str(), session.size());
    r.deadline = mono() + seconds * 1000; r.expiresAt = wall() + seconds * 1000; r.version = magic;
#else
    throw std::runtime_error("trace requires a POSIX edge");
#endif
}
TracePage ChannelTrace::read(const std::string& session, std::uint64_t after) {
#ifndef _WIN32
    Mapping m(path_, false); auto& r = *m.ring; check(r, session);
    if (after > r.seq) throw std::runtime_error("trace cursor ahead of session");
    TracePage page{r.deadline > mono(), after, 0, r.expiresAt, r.channel, {}};
    auto first = r.seq >= capacity ? r.seq - capacity + 1 : 1;
    page.overwritten = first > after + 1 ? first - after - 1 : 0;
    const auto end = std::min(r.seq, std::max(after + 1, first) + 127);
    for (auto n = std::max(after + 1, first); n <= end; ++n) {
        const auto& s = r.slots[(n - 1) % capacity];
        if (s.length > maxBytes || s.seq != n) throw std::runtime_error("trace ring corrupt");
        page.chunks.push_back({s.seq, s.ts, s.tx, s.originalLength, {s.bytes, s.bytes + s.length}}); page.cursor = n;
    }
    return page;
#else
    throw std::runtime_error("trace requires a POSIX edge");
#endif
}
void ChannelTrace::stop(const std::string& session) {
#ifndef _WIN32
    Mapping m(path_, false); check(*m.ring, session); m.ring->deadline = 0;
#endif
}
void ChannelTrace::capture(const std::string& channel, bool tx, const std::uint8_t* bytes, std::size_t size) noexcept {
    PreserveErrno savedError;
    if (!size || !bytes || channel.empty()) return;
#ifndef _WIN32
    try {
        Mapping m(path_, false); auto& r = *m.ring;
        if (r.version != magic || r.deadline <= mono() || channel != r.channel) return;
        auto n = r.seq + 1; auto& s = r.slots[(n - 1) % capacity];
        s.seq = 0; s.ts = wall(); s.tx = tx; s.originalLength = static_cast<std::uint32_t>(std::min<std::size_t>(size, UINT32_MAX)); s.length = std::min(size, maxBytes);
        std::memcpy(s.bytes, bytes, s.length);
        s.seq = n; r.seq = n; // Commit only a fully copied chunk while holding the inter-process lock.
    } catch (...) { /* Observation is best-effort: never block or throw into device I/O. */ }
#endif
}
void setTraceChannel(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(processMutex);
    processChannel = traceChannelKey(configPath);
}
std::string traceChannelKey(const std::string& configPath) {
#ifndef _WIN32
    char* resolved = realpath(configPath.c_str(), nullptr);
    if (!resolved) throw std::runtime_error("cannot resolve channel configuration");
    std::string result(resolved); free(resolved); return result;
#else
    return configPath;
#endif
}
void traceBytes(bool tx, const std::uint8_t* bytes, std::size_t size) noexcept {
    PreserveErrno savedError;
    try {
        std::unique_lock<std::mutex> lock(processMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        static ChannelTrace trace; trace.capture(processChannel, tx, bytes, size);
    } catch (...) { /* Even observer allocation/locking failure must not affect device I/O. */ }
}
}
