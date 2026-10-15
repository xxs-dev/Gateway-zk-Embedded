#ifdef NDEBUG
#undef NDEBUG
#endif
#include "edge_gateway/channel_trace.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <cerrno>

template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (...) { rejected = true; } assert(rejected); }
int main() {
    char dir[] = "/tmp/gateway-trace-test-XXXXXX"; assert(mkdtemp(dir));
    const auto path = std::string(dir) + "/trace.bin";
    edge_gateway::ChannelTrace trace(path);
    const std::uint8_t bytes[]{0x68, 0x04, 0x01, 0x16};
    trace.capture("channel-A", true, bytes, sizeof(bytes)); // default off, no file
    assert(access(path.c_str(), F_OK) != 0);
    rejects([&] { trace.start("channel-A", "bad/session", 10); });
    rejects([&] { trace.start("channel-A", "session-A", 301); });
    trace.start("channel-A", "session-A", 1);
    rejects([&] { trace.start("channel-B", "session-B", 60); });
    trace.capture("channel-B", true, bytes, sizeof(bytes));
    assert(trace.read("session-A", 0).chunks.empty());
    const auto child = fork(); assert(child >= 0);
    if (child == 0) { edge_gateway::ChannelTrace peer(path); peer.capture("channel-A", true, bytes, sizeof(bytes)); _exit(0); }
    int status = 0; waitpid(child, &status, 0); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    auto page = trace.read("session-A", 0);
    assert(page.chunks.size() == 1 && page.chunks[0].tx && page.chunks[0].bytes[0] == 0x68);
    rejects([&] { trace.read("wrong-session", 0); });
    rejects([&] { trace.stop("wrong-session"); });
    rejects([&] { trace.read("session-A", 999); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    trace.capture("channel-A", false, bytes, sizeof(bytes));
    assert(!trace.read("session-A", 1).active && trace.read("session-A", 1).chunks.empty());
    trace.start("channel-A", "session-B", 60);
    std::vector<std::uint8_t> large(3000, 0xFF);
    trace.capture("channel-A", false, large.data(), large.size());
    page = trace.read("session-B", 0);
    assert(page.chunks[0].bytes.size() == 2048 && page.chunks[0].originalLength == 3000);
    for (int i = 0; i < 600; ++i) trace.capture("channel-A", true, bytes, sizeof(bytes));
    page = trace.read("session-B", 0);
    assert(page.overwritten == 89 && page.chunks.size() == 128 && page.chunks[0].seq == 90);
    trace.stop("session-B"); trace.capture("channel-A", true, bytes, sizeof(bytes));
    assert(!trace.read("session-B", 601).active && trace.read("session-B", 601).chunks.empty());
    trace.start("channel-A", "session-C", 60);
    assert(trace.read("session-C", 0).chunks.empty()); // Old slots are not replayed.
    const int locked = open(path.c_str(), O_RDWR); assert(locked >= 0);
    assert(flock(locked, LOCK_EX | LOCK_NB) == 0);
    errno = EIO;
    trace.capture("channel-A", true, bytes, sizeof(bytes));
    assert(errno == EIO); // Busy observation never changes collection errors.
    close(locked);
    assert(trace.read("session-C", 0).chunks.empty());
    assert(chmod(path.c_str(), 0644) == 0);
    rejects([&] { trace.read("session-C", 0); });
    assert(chmod(path.c_str(), 0600) == 0);
    const auto alias = std::string(dir) + "/alias.bin";
    assert(link(path.c_str(), alias.c_str()) == 0);
    rejects([&] { trace.read("session-C", 0); });
    unlink(alias.c_str());
    assert(symlink(path.c_str(), alias.c_str()) == 0);
    edge_gateway::ChannelTrace linked(alias);
    rejects([&] { linked.read("session-C", 0); });
    unlink(alias.c_str());
    unlink(path.c_str()); rmdir(dir);
    std::cout << "channel trace: default-off, cross-process, isolation, ownership, expiry, truncation, overflow and stop passed\n";
}
