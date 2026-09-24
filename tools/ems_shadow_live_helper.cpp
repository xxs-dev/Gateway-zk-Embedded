#include "edge_gateway/json_value.hpp"
#include "edge_gateway/memory_point_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ems_shadow {
using edge_gateway::MemoryPointStore;
using edge_gateway::MemoryStoreOpenMode;
using edge_gateway::json::JsonValue;
constexpr const char* kName = "ems_shadow_20260924_pair";
constexpr int kRefreshMs = 200;
constexpr int kTtlMs = 1000;
constexpr std::size_t kMaxLine = 4096;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool target(std::uint32_t index) { return index >= 724060 && index <= 724065; }
bool writable(std::uint32_t index) {
    return index == 724000 || (index >= 724020 && index <= 724028) || target(index);
}

std::vector<std::uint32_t> inputs() {
    std::vector<std::uint32_t> result{724000};
    for (std::uint32_t i = 724020; i <= 724028; ++i) result.push_back(i);
    for (std::uint32_t i = 724060; i <= 724065; ++i) result.push_back(i);
    return result;
}

void keys(const JsonValue& value, const std::set<std::string>& expected) {
    require(value.isObject(), "command/value must be an object");
    std::set<std::string> found;
    for (const auto& member : value.asObject().values) {
        require(expected.count(member.key) && found.insert(member.key).second,
                "unknown or duplicate field");
    }
    require(found == expected, "missing field");
}

struct Descriptor {
    int fd = -1;
    ~Descriptor() { if (fd >= 0) close(fd); }
};

class Session {
public:
    explicit Session(const std::string& name) {
        require(name == kName, "only the exact shadow SHM name is allowed");
        const auto path = std::string("/") + kName;
        pinned_.fd = shm_open(path.c_str(), O_RDWR | O_CLOEXEC, 0);
        require(pinned_.fd >= 0, "shadow SHM must already exist");
        require(fstat(pinned_.fd, &identity_) == 0, "cannot identify shadow SHM");
        // OFD locks exclude another helper without blocking the runtime's flock.
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_len = 1;
        require(fcntl(pinned_.fd, F_OFD_SETLK, &lock) == 0,
                "another helper owns inputs, or kernel lacks OFD locks");
        store_.reset(new MemoryPointStore(name, MemoryStoreOpenMode::OpenExisting));
        check();
    }

    bool quitting() const { return quit_; }

    void tick(std::int64_t now) {
        require(now > 0 && now <= std::numeric_limits<std::int64_t>::max() - kTtlMs,
                "invalid wall clock");
        check();
        // Validate the complete refresh before writing any member of the batch.
        for (const auto& item : values_) {
            if (paused_ && target(item.first)) continue;
            const auto current = store_->getLatestByIndex(item.first, now);
            const auto previous = written_.find(item.first);
            if (previous == written_.end()) {
                require(!current || current->stale, "synthetic input already has a fresh writer");
            } else {
                const auto& old = previous->second;
                require(current && current->ts == old.ts && current->expireAt == old.expireAt &&
                        current->quality == old.quality && current->value == old.value,
                        "synthetic input changed outside helper");
            }
        }
        for (const auto& item : values_) {
            if (paused_ && target(item.first)) continue;
            edge_gateway::PointValue point;
            point.index = item.first;
            point.value = item.second;
            point.quality = 1;
            point.ts = now;
            point.expireAt = now + kTtlMs;
            store_->putLatest(point);
            written_[item.first] = point;
        }
    }

    std::string command(const std::string& line, std::int64_t now) {
        require(line.size() <= kMaxLine, "command too long");
        const auto root = edge_gateway::json::JsonParser(line, 5, 100).parse();
        const auto* operation = root.find("op");
        require(operation && operation->isString(), "missing string op");
        const auto& op = operation->asString();
        keys(root, op == "set" ? std::set<std::string>{"op", "values"} : std::set<std::string>{"op"});
        if (op == "set") {
            const auto& list = *root.find("values");
            require(list.isArray() && !list.asArray().values.empty() && list.asArray().values.size() <= 16,
                    "set requires 1..16 input values");
            std::map<std::uint32_t, double> update;
            for (const auto& row : list.asArray().values) {
                keys(*row, {"index", "value"});
                const auto& rawIndex = *row->find("index");
                const auto& rawValue = *row->find("value");
                require(rawIndex.isNumber() && rawValue.isNumber(), "index/value must be numeric");
                const auto index = rawIndex.asNumber();
                const auto value = rawValue.asNumber();
                require(std::isfinite(index) && index >= 1 && index <= 4294967295.0 &&
                        index == std::floor(index), "invalid index");
                const auto id = static_cast<std::uint32_t>(index);
                require(writable(id) && std::isfinite(value), "index outside synthetic scope or nonfinite value");
                require(update.emplace(id, value).second, "duplicate input index");
                if (id == 724000 || (id >= 724026 && id <= 724028))
                    require(value == 0 || value == 1, "enable/ready/interlock/override must be 0 or 1");
                if (id == 724020) require(value >= 0 && value <= 100, "SOC must be 0..100");
                if (id >= 724021 && id <= 724025) require(value >= 0, "capacity must be nonnegative");
            }
            for (const auto& item : update) values_[item.first] = item.second;
        } else if (op == "pause-targets") {
            paused_ = true;
        } else if (op == "resume-targets") {
            for (std::uint32_t i = 724060; i <= 724065; ++i)
                require(values_.count(i) != 0, "set all six targets before resuming");
            paused_ = false;
        } else if (op == "sample") {
            return sample(now);
        } else if (op == "quit") {
            quit_ = true;
        } else {
            throw std::runtime_error("unknown command");
        }
        check();
        return ack(op);
    }

    std::string ack(const std::string& op) const {
        return "{\"ok\":true,\"op\":\"" + op + "\",\"targetsPaused\":" + (paused_ ? "true}" : "false}");
    }

private:
    void check() const {
        Descriptor named;
        const auto path = std::string("/") + kName;
        named.fd = shm_open(path.c_str(), O_RDONLY | O_CLOEXEC, 0);
        struct stat current{};
        require(named.fd >= 0 && fstat(named.fd, &current) == 0 &&
                current.st_dev == identity_.st_dev && current.st_ino == identity_.st_ino &&
                current.st_size == identity_.st_size, "shadow SHM disappeared or was replaced/resized");
        require(store_->getStats().pendingWriteCount == 0, "shadow command queue is not empty");
    }

    void points(std::ostream& out, const std::vector<std::uint32_t>& indexes, std::int64_t now) const {
        const auto snapshots = store_->getLatestByIndexes(indexes, now);
        std::map<std::uint32_t, edge_gateway::StoredPointValue> found;
        for (const auto& point : snapshots) found.emplace(point.index, point);
        out << '[';
        bool first = true;
        for (const auto index : indexes) {
            out << (first ? "" : ",") << "{\"index\":" << index;
            first = false;
            const auto match = found.find(index);
            if (match == found.end()) { out << ",\"present\":false}"; continue; }
            const auto& point = match->second;
            out << ",\"present\":true,\"value\":";
            if (std::isfinite(point.value)) out << point.value;
            else out << "null";
            out << ",\"finite\":" << (std::isfinite(point.value) ? "true" : "false")
                << ",\"quality\":" << point.quality << ",\"ts\":" << point.ts
                << ",\"expireAt\":" << point.expireAt << ",\"stale\":" << (point.stale ? "true" : "false") << '}';
        }
        out << ']';
    }

    std::string sample(std::int64_t now) const {
        check();
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10)
            << "{\"ok\":true,\"op\":\"sample\",\"shm\":\"" << kName << "\",\"sampledAtMs\":" << now
            << ",\"targetsPaused\":" << (paused_ ? "true" : "false") << ",\"inputs\":";
        points(out, inputs(), now);
        out << ",\"points\":";
        points(out, {725000, 725001, 725002, 725003, 725004, 725005, 725006, 725007, 725008}, now);
        out << ",\"pendingWrites\":" << store_->getStats().pendingWriteCount << '}';
        check();
        return out.str();
    }

    Descriptor pinned_;
    struct stat identity_{};
    std::unique_ptr<MemoryPointStore> store_;
    std::map<std::uint32_t, double> values_;
    std::map<std::uint32_t, edge_gateway::PointValue> written_;
    bool paused_ = true;
    bool quit_ = false;
};

volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

int run(int argc, char** argv) {
    std::string name;
    int duration = 0;
    bool exclusive = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shm" && name.empty() && i + 1 < argc) name = argv[++i];
        else if (arg == "--exclusive-inputs" && !exclusive) exclusive = true;
        else if (arg == "--duration-seconds" && duration == 0 && i + 1 < argc) {
            const std::string value = argv[++i];
            require(!value.empty() && value.size() <= 4 && value.find_first_not_of("0123456789") == std::string::npos,
                    "invalid duration");
            duration = std::stoi(value);
            require(duration >= 1 && duration <= 2100, "duration must be 1..2100 seconds");
        } else throw std::runtime_error("expected --shm NAME --exclusive-inputs --duration-seconds 1..2100");
    }
    require(exclusive && duration > 0, "explicit SHM, duration and exclusive-inputs acknowledgment required");
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGALRM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
    alarm(static_cast<unsigned>(duration));
    Session session(name);
    std::cout << session.ack("ready") << std::endl;
    std::string pending;
    auto next = std::chrono::steady_clock::now();
    auto rateWindow = next;
    unsigned commands = 0;
    while (!stopping && !session.quitting()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) { session.tick(wallNow()); next = now + std::chrono::milliseconds(kRefreshMs); }
        if (now - rateWindow >= std::chrono::seconds(1)) { commands = 0; rateWindow = now; }
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int available = poll(&input, 1, 50);
        if (available < 0) { if (errno == EINTR) continue; throw std::runtime_error("stdin poll failed"); }
        if (!available) continue;
        require(!(input.revents & (POLLERR | POLLNVAL)), "stdin unavailable");
        char buffer[512];
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count == 0) { require(pending.empty(), "unterminated command at EOF"); break; }
        if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("stdin read failed"); }
        for (ssize_t i = 0; i < count && !session.quitting() && !stopping; ++i) {
            if (buffer[i] == '\n') {
                require(++commands <= 20, "command rate exceeds 20 per second");
                std::cout << session.command(pending, wallNow()) << std::endl;
                require(static_cast<bool>(std::cout), "stdout unavailable");
                pending.clear();
            } else {
                require(pending.size() < kMaxLine, "command too long");
                pending.push_back(buffer[i]);
            }
        }
    }
    alarm(0);
    return 0;
}
}  // namespace ems_shadow

#ifndef EMS_SHADOW_LIVE_HELPER_NO_MAIN
int main(int argc, char** argv) {
    try { return ems_shadow::run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "ems_shadow_live_helper: " << error.what() << '\n';
        return 2;
    }
}
#endif
