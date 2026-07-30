#include "edge_gateway/iec103_recording_command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::size_t kMaxCommandBytes = 8192;
constexpr std::size_t kMaxQueuedJobs = 32;
constexpr std::size_t kMaxRememberedJobs = 128;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isSafeRequestId(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    });
}

std::string percentEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '/') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ch);
        }
    }
    return out.str();
}

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            result.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) {
            throw std::invalid_argument("truncated percent escape");
        }
        const int high = hexDigit(value[i + 1]);
        const int low = hexDigit(value[i + 2]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("invalid percent escape");
        }
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return result;
}

std::vector<std::string> splitTabs(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('\t', start);
        result.push_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

long long parseInteger(const std::string& value, const char* field) {
    std::size_t consumed = 0;
    long long result = 0;
    try {
        result = std::stoll(value, &consumed, 10);
    } catch (...) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    return result;
}

std::string serializeStatus(const Iec103RecordingJobStatus& status) {
    std::ostringstream out;
    out << "V1\tSTATUS"
        << '\t' << percentEncode(status.requestId)
        << '\t' << percentEncode(status.action)
        << '\t' << percentEncode(status.stage)
        << '\t' << (status.retryable ? 1 : 0)
        << '\t' << status.fan
        << '\t' << percentEncode(status.files.cfgPath)
        << '\t' << status.files.cfgBytes
        << '\t' << percentEncode(status.files.datPath)
        << '\t' << status.files.datBytes
        << '\t' << percentEncode(status.errorCode)
        << '\t' << percentEncode(status.message)
        << '\t' << status.updatedAtMs
        << '\t' << status.records.size();
    for (const auto& record : status.records) {
        out << '\t' << record.fan
            << '\t' << record.state
            << '\t' << record.timestampMs
            << '\t' << percentEncode(record.rawTimeHex);
    }
    out << '\n';
    return out.str();
}

Iec103RecordingJobStatus parseStatus(const std::string& line) {
    const auto fields = splitTabs(line);
    if (fields.size() < 14 || fields[0] != "V1" || fields[1] != "STATUS") {
        throw std::runtime_error("invalid IEC103 recording command response");
    }
    Iec103RecordingJobStatus result;
    result.requestId = percentDecode(fields[2]);
    result.action = percentDecode(fields[3]);
    result.stage = percentDecode(fields[4]);
    result.retryable = parseInteger(fields[5], "retryable") != 0;
    result.fan = static_cast<int>(parseInteger(fields[6], "fan"));
    result.files.fan = result.fan;
    result.files.cfgPath = percentDecode(fields[7]);
    result.files.cfgBytes = static_cast<std::size_t>(parseInteger(fields[8], "cfgBytes"));
    result.files.datPath = percentDecode(fields[9]);
    result.files.datBytes = static_cast<std::size_t>(parseInteger(fields[10], "datBytes"));
    result.errorCode = percentDecode(fields[11]);
    result.message = percentDecode(fields[12]);
    result.updatedAtMs = parseInteger(fields[13], "updatedAtMs");
    if (fields.size() < 15) {
        throw std::runtime_error("missing IEC103 recording list count");
    }
    const auto count = static_cast<std::size_t>(parseInteger(fields[14], "recordCount"));
    if (count > 256 || fields.size() != 15 + count * 4) {
        throw std::runtime_error("invalid IEC103 recording list response");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = 15 + i * 4;
        Iec103DisturbanceRecord record;
        record.fan = static_cast<int>(parseInteger(fields[offset], "recordFan"));
        record.state = static_cast<int>(parseInteger(fields[offset + 1], "recordState"));
        record.timestampMs = parseInteger(fields[offset + 2], "recordTimestampMs");
        record.rawTimeHex = percentDecode(fields[offset + 3]);
        result.records.push_back(std::move(record));
    }
    return result;
}

Iec103RecordingJobStatus failedStatus(
    std::string requestId,
    std::string action,
    std::string errorCode,
    std::string message,
    bool retryable = false
) {
    Iec103RecordingJobStatus result;
    result.requestId = std::move(requestId);
    result.action = std::move(action);
    result.stage = "failed";
    result.retryable = retryable;
    result.errorCode = std::move(errorCode);
    result.message = std::move(message);
    result.updatedAtMs = nowMs();
    return result;
}

#ifndef _WIN32

void ensureDirectory(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::string current;
    std::size_t cursor = 0;
    if (path.front() == '/') {
        current = "/";
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const auto next = path.find('/', cursor);
        const auto part = path.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (mkdir(current.c_str(), 0750) != 0 && errno != EEXIST) {
                throw std::runtime_error("cannot create IEC103 command socket directory: " + current);
            }
        }
        if (next == std::string::npos) {
            break;
        }
        cursor = next + 1;
    }
}

void writeAll(int socketHandle, const std::string& value) {
    std::size_t written = 0;
    while (written < value.size()) {
        const auto count = send(socketHandle, value.data() + written, value.size() - written, MSG_NOSIGNAL);
        if (count <= 0) {
            throw std::runtime_error("IEC103 recording command write failed");
        }
        written += static_cast<std::size_t>(count);
    }
}

std::string readLine(int socketHandle) {
    std::string result;
    result.reserve(512);
    while (result.size() < kMaxCommandBytes) {
        char buffer[512];
        const auto count = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        result.append(buffer, static_cast<std::size_t>(count));
        const auto newline = result.find('\n');
        if (newline != std::string::npos) {
            result.resize(newline);
            return result;
        }
    }
    if (result.size() >= kMaxCommandBytes) {
        throw std::runtime_error("IEC103 recording command is too large");
    }
    return result;
}

int connectUnixSocket(const std::string& path) {
    const auto socketHandle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketHandle < 0) {
        throw std::runtime_error("cannot create IEC103 recording command socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        close(socketHandle);
        throw std::invalid_argument("IEC103 recording command socket path is too long");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const auto message = std::string("cannot connect IEC103 recording command socket: ") + std::strerror(errno);
        close(socketHandle);
        throw std::runtime_error(message);
    }
    return socketHandle;
}

Iec103RecordingJobStatus requestUnixSocket(const std::string& path, const std::string& request) {
    const auto socketHandle = connectUnixSocket(path);
    try {
        writeAll(socketHandle, request);
        shutdown(socketHandle, SHUT_WR);
        const auto response = readLine(socketHandle);
        close(socketHandle);
        return parseStatus(response);
    } catch (...) {
        close(socketHandle);
        throw;
    }
}

#endif

}  // namespace

std::string defaultIec103RecordingSocketPath(const std::string& configPath) {
    auto slash = configPath.find_last_of("/\\");
    auto name = slash == std::string::npos ? configPath : configPath.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    for (auto& ch : name) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_';
        if (!safe) {
            ch = '_';
        }
    }
    if (name.empty()) {
        name = "iec103";
    }
    return "/run/modbus-gateway/iec103/" + name + ".sock";
}

class Iec103RecordingCommandServer::Impl {
public:
    struct Job {
        std::string requestId;
        std::string action;
        int fan = -1;
    };

    Impl(
        std::shared_ptr<IecClient> targetClient,
        std::string targetSocketPath,
        std::string targetDirectory,
        int targetTimeoutMs
    ) : client(std::move(targetClient)),
        socketPathValue(std::move(targetSocketPath)),
        recordingDirectory(std::move(targetDirectory)),
        timeoutMs(std::max(1000, targetTimeoutMs)) {
        if (!client) {
            throw std::invalid_argument("IEC103 recording command client is required");
        }
        if (socketPathValue.empty()) {
            throw std::invalid_argument("IEC103 recording command socket path is required");
        }
    }

    ~Impl() {
        stop();
    }

    void start() {
#ifdef _WIN32
        throw std::runtime_error("IEC103 recording command socket is only supported on Linux");
#else
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) {
            return;
        }
        try {
            const auto slash = socketPathValue.find_last_of('/');
            ensureDirectory(slash == std::string::npos ? std::string() : socketPathValue.substr(0, slash));
            if (socketPathValue.size() >= sizeof(sockaddr_un{}.sun_path)) {
                throw std::invalid_argument("IEC103 recording command socket path is too long");
            }
            unlink(socketPathValue.c_str());
            listener = socket(AF_UNIX, SOCK_STREAM, 0);
            if (listener < 0) {
                throw std::runtime_error("cannot create IEC103 recording command listener");
            }
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, socketPathValue.c_str(), socketPathValue.size() + 1);
            if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
                listen(listener, 8) != 0) {
                throw std::runtime_error(
                    std::string("cannot listen on IEC103 recording command socket: ") + std::strerror(errno));
            }
            if (chmod(socketPathValue.c_str(), 0660) != 0) {
                throw std::runtime_error(
                    std::string("cannot set IEC103 recording command socket permissions: ") +
                    std::strerror(errno));
            }
            if (const auto* group = getgrnam("gateway")) {
                if (chown(socketPathValue.c_str(), static_cast<uid_t>(-1), group->gr_gid) != 0) {
                    throw std::runtime_error(
                        std::string("cannot set IEC103 recording command socket group: ") +
                        std::strerror(errno));
                }
            }
            client->setRecordingProgressCallback([this](const std::string& stage) {
                updateActiveStage(stage);
            });
            acceptThread = std::thread([this] { acceptLoop(); });
            workerThread = std::thread([this] { workerLoop(); });
        } catch (...) {
            running = false;
            if (listener >= 0) {
                close(listener);
                listener = -1;
            }
            unlink(socketPathValue.c_str());
            throw;
        }
#endif
    }

    void stop() {
        if (!running.exchange(false)) {
            return;
        }
#ifndef _WIN32
        if (listener >= 0) {
            shutdown(listener, SHUT_RDWR);
            close(listener);
            listener = -1;
        }
#endif
        queueChanged.notify_all();
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        if (workerThread.joinable()) {
            workerThread.join();
        }
        client->setRecordingProgressCallback({});
#ifndef _WIN32
        unlink(socketPathValue.c_str());
#endif
    }

    Iec103RecordingJobStatus submit(const Job& job) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto existing = statuses.find(job.requestId);
        if (existing != statuses.end()) {
            if (existing->second.action != job.action || existing->second.fan != job.fan) {
                return failedStatus(job.requestId, job.action, "REQUEST_ID_CONFLICT",
                    "requestId is already used by a different recording command");
            }
            return existing->second;
        }
        if (jobs.size() >= kMaxQueuedJobs) {
            return failedStatus(job.requestId, job.action, "QUEUE_FULL",
                "IEC103 recording command queue is full", true);
        }
        Iec103RecordingJobStatus status;
        status.requestId = job.requestId;
        status.action = job.action;
        status.stage = "accepted";
        status.fan = job.fan;
        status.updatedAtMs = nowMs();
        rememberLocked(status);
        jobs.push_back(job);
        queueChanged.notify_one();
        return status;
    }

    Iec103RecordingJobStatus find(const std::string& requestId) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = statuses.find(requestId);
        if (it == statuses.end()) {
            return failedStatus(requestId, "status", "REQUEST_NOT_FOUND",
                "IEC103 recording request was not found");
        }
        return it->second;
    }

    void updateActiveStage(const std::string& stage) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeRequestId.empty()) {
            return;
        }
        const auto it = statuses.find(activeRequestId);
        if (it != statuses.end()) {
            it->second.stage = stage;
            it->second.updatedAtMs = nowMs();
        }
    }

#ifndef _WIN32
    void acceptLoop() {
        while (running) {
            const auto accepted = accept(listener, nullptr, nullptr);
            if (accepted < 0) {
                if (!running) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            try {
                const auto response = handleCommand(readLine(accepted));
                writeAll(accepted, serializeStatus(response));
            } catch (const std::exception& ex) {
                try {
                    writeAll(accepted, serializeStatus(failedStatus(
                        "", "unknown", "INVALID_COMMAND", ex.what())));
                } catch (...) {
                }
            }
            close(accepted);
        }
    }
#endif

    Iec103RecordingJobStatus handleCommand(const std::string& line) {
        const auto fields = splitTabs(line);
        if (fields.size() < 3 || fields[0] != "V1") {
            throw std::invalid_argument("invalid IEC103 recording command frame");
        }
        const auto action = fields[1];
        const auto requestId = percentDecode(fields[2]);
        if (!isSafeRequestId(requestId)) {
            throw std::invalid_argument("invalid IEC103 recording requestId");
        }
        if (action == "STATUS" && fields.size() == 3) {
            return find(requestId);
        }
        if (action == "LIST" && fields.size() == 3) {
            return submit(Job{requestId, "list", -1});
        }
        if (action == "PULL" && fields.size() == 4) {
            const auto parsedFan = parseInteger(fields[3], "fan");
            if (parsedFan < 0 || parsedFan > 65535) {
                throw std::invalid_argument("IEC103 recording FAN is out of range");
            }
            return submit(Job{requestId, "pull", static_cast<int>(parsedFan)});
        }
        throw std::invalid_argument("unsupported IEC103 recording command");
    }

    void workerLoop() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                queueChanged.wait(lock, [this] { return !running || !jobs.empty(); });
                if (!running && jobs.empty()) {
                    break;
                }
                job = jobs.front();
                jobs.pop_front();
                activeRequestId = job.requestId;
                auto& status = statuses[job.requestId];
                status.stage = "device_connecting";
                status.updatedAtMs = nowMs();
            }
            try {
                if (job.action == "list") {
                    updateActiveStage("pulling_directory");
                    const auto records = client->listDisturbanceRecords(timeoutMs);
                    std::lock_guard<std::mutex> lock(mutex);
                    auto& status = statuses[job.requestId];
                    status.records = records;
                    status.stage = "completed";
                    status.message = "recording directory ready";
                    status.updatedAtMs = nowMs();
                } else {
                    const auto files = client->pullComtradeRecording(
                        job.fan, recordingDirectory, timeoutMs);
                    std::lock_guard<std::mutex> lock(mutex);
                    auto& status = statuses[job.requestId];
                    status.files = files;
                    status.stage = "local_ready";
                    status.message = "COMTRADE recording is ready locally";
                    status.updatedAtMs = nowMs();
                }
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(mutex);
                auto& status = statuses[job.requestId];
                status.stage = "failed";
                status.retryable = true;
                status.errorCode = "DEVICE_IO_FAILED";
                status.message = ex.what();
                status.updatedAtMs = nowMs();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                activeRequestId.clear();
            }
        }
    }

    void rememberLocked(const Iec103RecordingJobStatus& status) {
        while (statusOrder.size() >= kMaxRememberedJobs) {
            const auto oldest = statusOrder.front();
            statusOrder.pop_front();
            statuses.erase(oldest);
        }
        statuses[status.requestId] = status;
        statusOrder.push_back(status.requestId);
    }

    std::shared_ptr<IecClient> client;
    std::string socketPathValue;
    std::string recordingDirectory;
    int timeoutMs = 30000;
    std::atomic<bool> running{false};
    mutable std::mutex mutex;
    std::condition_variable queueChanged;
    std::deque<Job> jobs;
    std::unordered_map<std::string, Iec103RecordingJobStatus> statuses;
    std::deque<std::string> statusOrder;
    std::string activeRequestId;
    std::thread acceptThread;
    std::thread workerThread;
#ifndef _WIN32
    int listener = -1;
#endif
};

Iec103RecordingCommandServer::Iec103RecordingCommandServer(
    std::shared_ptr<IecClient> client,
    std::string socketPath,
    std::string recordingDirectory,
    int timeoutMs
) : impl_(new Impl(
        std::move(client),
        std::move(socketPath),
        std::move(recordingDirectory),
        timeoutMs)) {
}

Iec103RecordingCommandServer::~Iec103RecordingCommandServer() = default;

void Iec103RecordingCommandServer::start() {
    impl_->start();
}

void Iec103RecordingCommandServer::stop() {
    impl_->stop();
}

const std::string& Iec103RecordingCommandServer::socketPath() const {
    return impl_->socketPathValue;
}

Iec103RecordingJobStatus Iec103RecordingCommandClient::submitList(
    const std::string& socketPath,
    const std::string& requestId
) {
#ifdef _WIN32
    (void)socketPath;
    (void)requestId;
    throw std::runtime_error("IEC103 recording command socket is only supported on Linux");
#else
    return requestUnixSocket(socketPath, "V1\tLIST\t" + percentEncode(requestId) + "\n");
#endif
}

Iec103RecordingJobStatus Iec103RecordingCommandClient::submitPull(
    const std::string& socketPath,
    const std::string& requestId,
    int fan
) {
#ifdef _WIN32
    (void)socketPath;
    (void)requestId;
    (void)fan;
    throw std::runtime_error("IEC103 recording command socket is only supported on Linux");
#else
    return requestUnixSocket(socketPath,
        "V1\tPULL\t" + percentEncode(requestId) + "\t" + std::to_string(fan) + "\n");
#endif
}

Iec103RecordingJobStatus Iec103RecordingCommandClient::status(
    const std::string& socketPath,
    const std::string& requestId
) {
#ifdef _WIN32
    (void)socketPath;
    (void)requestId;
    throw std::runtime_error("IEC103 recording command socket is only supported on Linux");
#else
    return requestUnixSocket(socketPath, "V1\tSTATUS\t" + percentEncode(requestId) + "\n");
#endif
}

}  // namespace edge_gateway
