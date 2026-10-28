#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include "edge_gateway/filesystem_compat.hpp"
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace {
#ifndef _WIN32
struct File {
    int fd;
    explicit File(int value) : fd(value) { if (fd < 0) throw std::runtime_error("EMS control mode file unavailable"); }
    ~File() { close(fd); }
};
std::shared_ptr<File> lock(const std::string& path, bool exclusive) {
    auto result = std::make_shared<File>(open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (flock(result->fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        throw std::runtime_error("EMS control mode busy; retry after current command completes");
    return result;
}
#endif
}

EmsControlMode::EmsControlMode(std::string path) : path_(std::move(path)) {
    if (path_.empty()) throw std::invalid_argument("EMS control mode path is required");
}

EmsControlModeState EmsControlMode::read() const {
#ifdef _WIN32
    throw std::runtime_error("EMS control mode requires edge runtime");
#else
    const int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return {};
    File file(fd);
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 4096)
        throw std::runtime_error("Invalid EMS control mode state");
    std::string text(static_cast<std::size_t>(st.st_size), '\0');
    if (::read(fd, &text[0], text.size()) != static_cast<ssize_t>(text.size()))
        throw std::runtime_error("Incomplete EMS control mode state");
    auto root = json::JsonParser(text, 4, 32).parse();
    const auto version = root.find("version"), mode = root.find("mode"), cutoff = root.find("denyAcceptedThrough");
    if (!root.isObject() || !version || !version->isNumber() || version->asNumber() != 1 ||
        !mode || !mode->isString() || (mode->asString() != "remote" && mode->asString() != "local") ||
        !cutoff || !cutoff->isNumber() || !std::isfinite(cutoff->asNumber()) ||
        cutoff->asNumber() < 0 || cutoff->asNumber() > 9007199254740991.0 ||
        std::floor(cutoff->asNumber()) != cutoff->asNumber())
        throw std::runtime_error("Invalid EMS control mode state");
    return {mode->asString() == "remote", static_cast<std::int64_t>(cutoff->asNumber())};
#endif
}

void EmsControlMode::admitPlatform(std::int64_t acceptedAt) const {
    const auto state = read();
    if (!state.remote) throw std::runtime_error("EMS is in local mode; platform control is disabled");
    if (acceptedAt <= state.denyAcceptedThrough)
        throw std::runtime_error("Platform command revoked by EMS mode transition");
}

std::shared_ptr<void> EmsControlMode::dispatchPermit(const PendingWriteCommand& command) const {
    if (!command.platformControl) return {};
#ifdef _WIN32
    throw std::runtime_error("EMS platform dispatch requires edge runtime");
#else
    auto token = lock(path_, false);
    admitPlatform(command.acceptedAt);
    return token;
#endif
}

void EmsControlMode::setRemote(bool remote, std::int64_t nowMs) const {
#ifdef _WIN32
    throw std::runtime_error("EMS control mode requires edge runtime");
#else
    if (nowMs <= 0) throw std::invalid_argument("EMS mode timestamp is invalid");
    auto token = lock(path_, true);
    const auto old = read();
    const auto cutoff = remote ? old.denyAcceptedThrough : std::max(old.denyAcceptedThrough, nowMs);
    const auto text = std::string("{\"version\":1,\"mode\":\"") + (remote ? "remote" : "local") +
        "\",\"denyAcceptedThrough\":" + std::to_string(cutoff) + ",\"changedAt\":" + std::to_string(nowMs) +
        ",\"actor\":\"authenticated-local-operator\"}\n";
    const auto temp = path_ + ".next";
    File file(open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (write(file.fd, text.data(), text.size()) != static_cast<ssize_t>(text.size()) || fsync(file.fd))
        throw std::runtime_error("Could not persist EMS control mode");
    if (rename(temp.c_str(), path_.c_str())) throw std::runtime_error("Could not commit EMS control mode");
    File directory(open(filesystem::path(path_).parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fsync(directory.fd)) throw std::runtime_error("EMS control mode directory sync failed; verify state");
#endif
}
} // namespace edge_gateway
