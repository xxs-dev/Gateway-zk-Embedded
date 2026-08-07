#include "edge_gateway/iec103_recording_transfer_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <direct.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/iec103_recording_command.hpp"

namespace edge_gateway {

namespace {

constexpr std::size_t kMaxHttpResponseBytes = 1024 * 1024;
constexpr std::size_t kMaxQueueLineBytes = 16 * 1024;

class TransferFailure : public std::runtime_error {
public:
    TransferFailure(std::string message, bool retryable)
        : std::runtime_error(std::move(message)), retryable_(retryable) {}

    bool retryable() const { return retryable_; }

private:
    bool retryable_;
};

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string escapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    std::ostringstream encoded;
                    encoded << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(ch);
                    result += encoded.str();
                } else {
                    result.push_back(static_cast<char>(ch));
                }
        }
    }
    return result;
}

class JsonFieldReader {
public:
    explicit JsonFieldReader(const std::string& text) : text_(text) {}

    bool getString(const char* key, std::string* value) const {
        auto cursor = valuePosition(key);
        if (cursor == std::string::npos || cursor >= text_.size() || text_[cursor] != '"') {
            return false;
        }
        ++cursor;
        std::string result;
        while (cursor < text_.size()) {
            const char ch = text_[cursor++];
            if (ch == '"') {
                *value = result;
                return true;
            }
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (cursor >= text_.size()) {
                return false;
            }
            const char escaped = text_[cursor++];
            switch (escaped) {
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case '\\': result.push_back('\\'); break;
                case '"': result.push_back('"'); break;
                default: result.push_back(escaped); break;
            }
        }
        return false;
    }

    bool getInt(const char* key, int* value) const {
        std::int64_t parsed = 0;
        if (!getInt64(key, &parsed) || parsed < -2147483648LL || parsed > 2147483647LL) {
            return false;
        }
        *value = static_cast<int>(parsed);
        return true;
    }

    bool getInt64(const char* key, std::int64_t* value) const {
        auto cursor = valuePosition(key);
        if (cursor == std::string::npos) {
            return false;
        }
        const auto begin = cursor;
        if (cursor < text_.size() && text_[cursor] == '-') {
            ++cursor;
        }
        while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == begin || (cursor == begin + 1 && text_[begin] == '-')) {
            return false;
        }
        try {
            *value = std::stoll(text_.substr(begin, cursor - begin));
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::size_t valuePosition(const char* key) const {
        const std::string needle = std::string("\"") + key + "\"";
        auto cursor = text_.find(needle);
        if (cursor == std::string::npos) {
            return cursor;
        }
        cursor += needle.size();
        while (cursor < text_.size() && std::isspace(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text_.size() || text_[cursor] != ':') {
            return std::string::npos;
        }
        ++cursor;
        while (cursor < text_.size() && std::isspace(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        return cursor;
    }

    const std::string& text_;
};

bool isSafeId(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
                ch == '_' || ch == '-' || ch == '.' || ch == ':';
        });
}

bool hasControlCharacter(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7F;
    });
}

std::string percentEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == ':') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ch);
        }
    }
    return out.str();
}

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
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
            throw std::runtime_error("invalid recording queue percent escape");
        }
        const int high = hexDigit(value[i + 1]);
        const int low = hexDigit(value[i + 2]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("invalid recording queue percent escape");
        }
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return result;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

long long parseInteger(const std::string& value) {
    std::size_t consumed = 0;
    const auto result = std::stoll(value, &consumed, 10);
    if (consumed != value.size()) {
        throw std::runtime_error("invalid recording queue integer");
    }
    return result;
}

int makeDirectory(const std::string& path) {
#ifdef _WIN32
    return _mkdir(path.c_str());
#else
    return mkdir(path.c_str(), 0750);
#endif
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    std::size_t cursor = 0;
    if (path.front() == '/' || path.front() == '\\') {
        current.assign(1, path.front());
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const auto next = path.find_first_of("/\\", cursor);
        const auto part = path.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/' && current.back() != '\\') current.push_back('/');
            current += part;
            if (makeDirectory(current) != 0 && errno != EEXIST) {
                throw std::runtime_error("cannot create recording transfer directory: " + current);
            }
        }
        if (next == std::string::npos) break;
        cursor = next + 1;
    }
}

std::string dirnameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string basenameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::uint64_t fileSize(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open recording file: " + path);
    const auto size = input.tellg();
    if (size < 0) throw std::runtime_error("cannot determine recording file size: " + path);
    return static_cast<std::uint64_t>(size);
}

std::string readSmallFile(const std::string& path, std::size_t limit) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return {};
    std::string result;
    char buffer[4096];
    while (input && result.size() <= limit) {
        input.read(buffer, sizeof(buffer));
        result.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    if (result.size() > limit) result.resize(limit);
    return result;
}

void writePrivateFile(const std::string& path, const std::string& content) {
    ensureDirectory(dirnameOf(path));
#ifndef _WIN32
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("cannot write recording transfer file: " + path);
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto count = write(
            descriptor,
            content.data() + offset,
            static_cast<std::size_t>(content.size() - offset)
        );
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor);
            std::remove(path.c_str());
            throw std::runtime_error("cannot write recording transfer file: " + path);
        }
        offset += static_cast<std::size_t>(count);
    }
    const int syncResult = fsync(descriptor);
    const int closeResult = close(descriptor);
    if (syncResult != 0 || closeResult != 0) {
        std::remove(path.c_str());
        throw std::runtime_error("cannot flush recording transfer file: " + path);
    }
#else
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write recording transfer file: " + path);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) throw std::runtime_error("cannot flush recording transfer file: " + path);
#endif
}

void replaceFile(const std::string& temporary, const std::string& target) {
#ifdef _WIN32
    std::remove(target.c_str());
#endif
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot finalize recording transfer file: " + target);
    }
}

std::atomic<unsigned long long> g_tempCounter{0};

std::string temporaryPath(const std::string& directory, const std::string& prefix) {
    std::ostringstream path;
    path << directory << "/" << prefix << "-" << currentTimeMs() << "-" << ++g_tempCounter << ".tmp";
    return path.str();
}

std::vector<int> parsePartNumbers(const std::string& json, const std::string& kind) {
    const std::string needle = std::string("\"") + kind + "\"";
    auto cursor = json.find(needle);
    if (cursor == std::string::npos) return {};
    cursor = json.find('[', cursor + needle.size());
    if (cursor == std::string::npos) return {};
    const auto end = json.find(']', cursor + 1);
    if (end == std::string::npos) throw std::runtime_error("invalid recording parts response");
    std::vector<int> result;
    ++cursor;
    while (cursor < end) {
        while (cursor < end && (std::isspace(static_cast<unsigned char>(json[cursor])) != 0 || json[cursor] == ',')) ++cursor;
        if (cursor >= end) break;
        const auto begin = cursor;
        while (cursor < end && std::isdigit(static_cast<unsigned char>(json[cursor])) != 0) ++cursor;
        if (begin == cursor) throw std::runtime_error("invalid recording part number");
        result.push_back(std::stoi(json.substr(begin, cursor - begin)));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool containsPart(const std::vector<int>& values, int partNo) {
    return std::binary_search(values.begin(), values.end(), partNo);
}

void requireHttpSuccess(const RecordingHttpResponse& response, const std::string& operation) {
    if (response.statusCode >= 200 && response.statusCode < 300) return;
    const bool retryable = response.statusCode == 0 || response.statusCode == 408 ||
        response.statusCode == 409 || response.statusCode == 425 || response.statusCode == 429 ||
        response.statusCode >= 500;
    throw TransferFailure(
        operation + " failed with HTTP " + std::to_string(response.statusCode) +
            (response.body.empty() ? std::string() : ": " + response.body.substr(0, 512)),
        retryable
    );
}

#ifndef _WIN32
std::string sha256File(const std::string& path) {
    int outputPipe[2] = {-1, -1};
    if (pipe(outputPipe) != 0) throw std::runtime_error("cannot create sha256 pipe");
    const pid_t pid = fork();
    if (pid < 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        throw std::runtime_error("cannot fork sha256sum");
    }
    if (pid == 0) {
        dup2(outputPipe[1], STDOUT_FILENO);
        close(outputPipe[0]);
        close(outputPipe[1]);
        execlp("sha256sum", "sha256sum", "--", path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(outputPipe[1]);
    std::string output;
    char buffer[256];
    ssize_t count = 0;
    while ((count = read(outputPipe[0], buffer, sizeof(buffer))) > 0 && output.size() < 1024) {
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(outputPipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.size() < 64) {
        throw std::runtime_error("sha256sum failed for recording file");
    }
    const auto digest = output.substr(0, 64);
    if (!std::all_of(digest.begin(), digest.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) {
        throw std::runtime_error("sha256sum returned invalid digest");
    }
    std::string normalized = digest;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}
#else
std::string sha256File(const std::string&) {
    throw std::runtime_error("recording SHA256 is only supported on Linux");
}
#endif

}  // namespace

CurlRecordingHttpClient::CurlRecordingHttpClient(
    std::string executable,
    std::string workDirectory,
    bool allowInsecureHttp,
    int timeoutSec
) : executable_(std::move(executable)),
    workDirectory_(std::move(workDirectory)),
    allowInsecureHttp_(allowInsecureHttp),
    timeoutSec_(std::max(10, timeoutSec)) {
    if (executable_.empty() || executable_.size() > 512 || hasControlCharacter(executable_)) {
        throw std::invalid_argument("invalid curl executable");
    }
    ensureDirectory(workDirectory_);
}

RecordingHttpResponse CurlRecordingHttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& bodyFile
) {
#ifdef _WIN32
    (void)method; (void)url; (void)headers; (void)bodyFile;
    throw std::runtime_error("recording HTTPS upload is only supported on Linux");
#else
    const bool https = url.find("https://") == 0;
    const bool http = url.find("http://") == 0;
    if (url.size() > 2048 || hasControlCharacter(url) || (!https && !(allowInsecureHttp_ && http))) {
        throw TransferFailure("recording upload URL is not allowed", false);
    }
    if (method != "GET" && method != "POST" && method != "PUT") {
        throw TransferFailure("recording HTTP method is not allowed", false);
    }
    std::ostringstream headerText;
    for (const auto& header : headers) {
        if (header.empty() || header.size() > 2048 || hasControlCharacter(header) ||
            header.find('"') != std::string::npos || header.find('\\') != std::string::npos) {
            throw TransferFailure("recording HTTP header is invalid", false);
        }
        headerText << "header = \"" << header << "\"\n";
    }
    const auto headerFile = temporaryPath(workDirectory_, "headers");
    const auto responseFile = temporaryPath(workDirectory_, "response");
    writePrivateFile(headerFile, headerText.str());

    int outputPipe[2] = {-1, -1};
    if (pipe(outputPipe) != 0) {
        std::remove(headerFile.c_str());
        throw std::runtime_error("cannot create curl status pipe");
    }
    std::vector<std::string> arguments = {
        executable_,
        "--silent",
        "--show-error",
        "--request", method,
        "--proto", allowInsecureHttp_ ? "=http,https" : "=https",
        "--connect-timeout", "15",
        "--max-time", std::to_string(timeoutSec_),
        "--config", headerFile,
        "--output", responseFile,
        "--write-out", "%{http_code}"
    };
    if (!bodyFile.empty()) {
        arguments.push_back("--data-binary");
        arguments.push_back("@" + bodyFile);
    }
    arguments.push_back("--");
    arguments.push_back(url);

    const pid_t pid = fork();
    if (pid < 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        std::remove(headerFile.c_str());
        throw std::runtime_error("cannot fork curl uploader");
    }
    if (pid == 0) {
        dup2(outputPipe[1], STDOUT_FILENO);
        close(outputPipe[0]);
        close(outputPipe[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(executable_.c_str(), argv.data());
        _exit(127);
    }
    close(outputPipe[1]);
    int childStatus = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec_ + 10);
    while (waitpid(pid, &childStatus, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGTERM);
            waitpid(pid, &childStatus, 0);
            close(outputPipe[0]);
            std::remove(headerFile.c_str());
            std::remove(responseFile.c_str());
            throw TransferFailure("recording HTTP request timed out", true);
        }
        usleep(100 * 1000);
    }
    std::string statusText;
    char statusBuffer[32];
    ssize_t count = 0;
    while ((count = read(outputPipe[0], statusBuffer, sizeof(statusBuffer))) > 0) {
        statusText.append(statusBuffer, static_cast<std::size_t>(count));
    }
    close(outputPipe[0]);
    RecordingHttpResponse response;
    response.body = readSmallFile(responseFile, kMaxHttpResponseBytes);
    std::remove(headerFile.c_str());
    std::remove(responseFile.c_str());
    if (statusText.size() >= 3 && std::isdigit(static_cast<unsigned char>(statusText[0])) != 0) {
        response.statusCode = std::atoi(statusText.substr(0, 3).c_str());
    }
    if (!WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0) {
        throw TransferFailure(
            "curl recording request failed with exit=" +
                std::to_string(WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1),
            true
        );
    }
    return response;
#endif
}

Iec103RecordingTransferService::Iec103RecordingTransferService(
    SystemMonitorConfig::RecordingTransferConfig config,
    MqttConfig mqttConfig,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::string machineCode,
    std::vector<std::string> configFiles,
    std::shared_ptr<IRecordingHttpClient> httpClient
) : config_(std::move(config)),
    mqttConfig_(std::move(mqttConfig)),
    publisher_(std::move(publisher)),
    machineCode_(std::move(machineCode)),
    configFiles_(std::move(configFiles)),
    httpClient_(std::move(httpClient)) {
    if (!publisher_) throw std::invalid_argument("recording transfer MQTT publisher is required");
    ensureDirectory(config_.workDirectory);
    ensureDirectory(dirnameOf(config_.queueFile));
    if (!httpClient_) {
        httpClient_ = std::make_shared<CurlRecordingHttpClient>(
            config_.curlExecutable,
            config_.workDirectory,
            config_.allowInsecureHttp,
            config_.requestTimeoutSec
        );
    }
    loadQueue();
}

Iec103RecordingTransferService::~Iec103RecordingTransferService() {
    stop();
}

void Iec103RecordingTransferService::start() {
    if (!config_.enabled) return;
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    workerThread_ = std::thread([this] { loop(); });
}

void Iec103RecordingTransferService::stop() {
    if (!running_.exchange(false)) return;
    if (workerThread_.joinable()) workerThread_.join();
}

void Iec103RecordingTransferService::handleRequest(const std::string& payload, std::int64_t nowMs) {
    JsonFieldReader json(payload);
    Task task;
    std::string requestMachineCode;
    if (!json.getString("requestId", &task.requestId) || !isSafeId(task.requestId))
        throw std::invalid_argument("invalid recording requestId");
    if (!json.getString("machineCode", &requestMachineCode) || requestMachineCode != machineCode_)
        throw std::invalid_argument("recording request machineCode mismatch");
    if (!json.getString("deviceCode", &task.deviceCode) || !isSafeId(task.deviceCode))
        throw std::invalid_argument("invalid recording deviceCode");
    if (!json.getString("action", &task.action) || (task.action != "list" && task.action != "pull"))
        throw std::invalid_argument("recording action must be list or pull");
    if (task.action == "pull" && (!json.getInt("fan", &task.fan) || task.fan < 0 || task.fan > 65535))
        throw std::invalid_argument("recording FAN is invalid");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = tasks_.find(task.requestId);
        if (existing != tasks_.end()) {
            if (existing->second.action != task.action || existing->second.deviceCode != task.deviceCode ||
                existing->second.fan != task.fan) {
                throw std::invalid_argument("recording requestId conflicts with an existing task");
            }
            task = existing->second;
        }
    }
    if (!task.socketPath.empty() || task.stage == "completed") {
        const auto completed = task.stage == "completed";
        publishReply(task, task.stage,
            completed ? "recording task already completed" : "recording task already exists");
        publishStatus(task, task.stage,
            completed ? "recording task already completed" : "recording task already exists");
        return;
    }

    bool matched = false;
    std::string lastConfigError;
    for (const auto& configFile : configFiles_) {
        try {
            const auto device = ConfigLoader::loadFromFile(configFile);
            if (device.protocol.iec.transportMode != "am5se_passive_tcp") continue;
            bool deviceMatches = device.meterCode == task.deviceCode;
            for (const auto& meter : device.meters) deviceMatches = deviceMatches || meter.meterCode == task.deviceCode;
            if (!deviceMatches) continue;
            task.socketPath = device.protocol.iec.recordingCommandSocket.empty()
                ? defaultIec103RecordingSocketPath(configFile)
                : device.protocol.iec.recordingCommandSocket;
            matched = true;
            break;
        } catch (const std::exception& ex) {
            lastConfigError = configFile + ": " + ex.what();
        }
    }
    if (!matched) {
        std::string message = "AM5SE IEC103 network device configuration was not found";
        if (!lastConfigError.empty()) message += "; last config error: " + lastConfigError;
        throw std::invalid_argument(message);
    }

    if (task.action == "pull") {
        if (!json.getString("sessionId", &task.sessionId) || !isSafeId(task.sessionId))
            throw std::invalid_argument("invalid recording upload sessionId");
        if (!json.getString("baseUrl", &task.baseUrl) || task.baseUrl.size() > 2048 || hasControlCharacter(task.baseUrl))
            throw std::invalid_argument("invalid recording upload baseUrl");
        const bool https = task.baseUrl.find("https://") == 0;
        const bool http = task.baseUrl.find("http://") == 0;
        if (!https && !(config_.allowInsecureHttp && http))
            throw std::invalid_argument("recording upload baseUrl must use HTTPS");
        if (!json.getString("token", &task.token) || task.token.empty() || task.token.size() > 512 || hasControlCharacter(task.token))
            throw std::invalid_argument("invalid recording upload token");
        if (!json.getInt64("expireAtMs", &task.expireAtMs) || task.expireAtMs <= nowMs)
            throw std::invalid_argument("recording upload token is already expired");
        if (!json.getInt("partSize", &task.partSize) || task.partSize < 64 * 1024 || task.partSize > 8 * 1024 * 1024)
            throw std::invalid_argument("recording upload partSize is invalid");
    }
    task.nextRetryAtMs = nowMs;

    bool inserted = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = tasks_.find(task.requestId);
        if (existing != tasks_.end()) {
            if (existing->second.action != task.action || existing->second.deviceCode != task.deviceCode ||
                existing->second.fan != task.fan) {
                throw std::invalid_argument("recording requestId conflicts with an existing task");
            }
            task = existing->second;
            inserted = false;
        } else {
            tasks_[task.requestId] = task;
            saveQueueLocked();
        }
    }
    publishReply(task, inserted ? "accepted" : task.stage,
        inserted ? "recording task accepted" : "recording task already exists");
    publishStatus(task, task.stage,
        inserted ? "recording task accepted" : "recording task already exists");
}

void Iec103RecordingTransferService::handleAck(const std::string& payload, std::int64_t nowMs) {
    JsonFieldReader json(payload);
    std::string requestId;
    std::string recordingId;
    std::string machineCode;
    std::string status;
    if (!json.getString("requestId", &requestId) || !isSafeId(requestId))
        throw std::invalid_argument("invalid recording ACK requestId");
    if (!json.getString("recordingId", &recordingId) || !isSafeId(recordingId))
        throw std::invalid_argument("invalid recording ACK recordingId");
    if (!json.getString("machineCode", &machineCode) || machineCode != machineCode_)
        throw std::invalid_argument("recording ACK machineCode mismatch");
    if (!json.getString("status", &status) || status != "completed")
        throw std::invalid_argument("recording ACK status must be completed");

    Task task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = tasks_.find(requestId);
        if (it == tasks_.end()) return;
        task = it->second;
        if (task.stage == "completed") return;
        if (task.action != "pull" ||
            (task.stage != "uploading" && task.stage != "awaiting_ack")) return;
        auto& completed = it->second;
        completed.stage = "completed";
        completed.sessionId.clear();
        completed.baseUrl.clear();
        completed.token.clear();
        completed.expireAtMs = 0;
        completed.partSize = 0;
        completed.socketPath.clear();
        completed.cfgPath.clear();
        completed.cfgBytes = 0;
        completed.cfgSha256.clear();
        completed.datPath.clear();
        completed.datBytes = 0;
        completed.datSha256.clear();
        completed.retryCount = 0;
        completed.nextRetryAtMs = nowMs;
        completed.lastError.clear();
        pruneCompletedLocked();
        saveQueueLocked();
    }
    std::remove(task.cfgPath.c_str());
    std::remove(task.datPath.c_str());
    publishStatus(task, "completed", "platform acknowledged recording upload");
    (void)nowMs;
}

bool Iec103RecordingTransferService::runPendingOnce(std::int64_t nowMs) {
    Task task;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : tasks_) {
            if (entry.second.stage == "failed" || entry.second.stage == "awaiting_ack" ||
                entry.second.stage == "completed" ||
                entry.second.nextRetryAtMs > nowMs) continue;
            task = entry.second;
            found = true;
            break;
        }
    }
    if (!found) return false;
    try {
        processTask(task, nowMs);
    } catch (const TransferFailure& ex) {
        scheduleRetry(task, ex.what(), nowMs, ex.retryable());
    } catch (const std::exception& ex) {
        scheduleRetry(task, ex.what(), nowMs, true);
    }
    return true;
}

std::size_t Iec103RecordingTransferService::pendingTaskCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(tasks_.begin(), tasks_.end(), [](const auto& entry) {
        return entry.second.stage != "completed";
    }));
}

void Iec103RecordingTransferService::loop() {
    while (running_) {
        if (!runPendingOnce(currentTimeMs())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

void Iec103RecordingTransferService::processTask(Task task, std::int64_t nowMs) {
    if (task.stage == "accepted" || task.stage == "local_wait") {
        processLocalCommand(task, nowMs);
        return;
    }
    if (task.action == "pull" && (task.stage == "local_ready" || task.stage == "uploading")) {
        processUpload(task, nowMs);
    }
}

void Iec103RecordingTransferService::processLocalCommand(Task& task, std::int64_t nowMs) {
    Iec103RecordingJobStatus local;
    if (task.stage == "accepted") {
        local = task.action == "list"
            ? Iec103RecordingCommandClient::submitList(task.socketPath, task.requestId)
            : Iec103RecordingCommandClient::submitPull(task.socketPath, task.requestId, task.fan);
        task.stage = "local_wait";
        task.nextRetryAtMs = nowMs + 250;
        task.lastError.clear();
        applyTask(task);
        publishStatus(task, local.stage, local.message.empty() ? "device recording command accepted" : local.message);
        if (!local.terminal()) return;
    } else {
        local = Iec103RecordingCommandClient::status(task.socketPath, task.requestId);
    }

    if (local.stage == "failed") {
        if (local.errorCode == "REQUEST_NOT_FOUND") {
            task.stage = "accepted";
            task.nextRetryAtMs = nowMs;
            applyTask(task);
            return;
        }
        throw TransferFailure(local.message.empty() ? "IEC103 recording command failed" : local.message,
            local.retryable);
    }
    if (!local.terminal()) {
        task.nextRetryAtMs = nowMs + 250;
        applyTask(task);
        publishStatus(task, local.stage, local.message);
        return;
    }

    if (task.action == "list") {
        std::ostringstream reply;
        reply << "{\"requestId\":\"" << escapeJson(task.requestId)
              << "\",\"machineCode\":\"" << escapeJson(machineCode_)
              << "\",\"deviceCode\":\"" << escapeJson(task.deviceCode)
              << "\",\"action\":\"list\",\"success\":true,\"stage\":\"completed\",\"records\":[";
        for (std::size_t i = 0; i < local.records.size(); ++i) {
            if (i > 0) reply << ',';
            reply << "{\"fan\":" << local.records[i].fan
                  << ",\"state\":" << local.records[i].state
                  << ",\"timestampMs\":" << local.records[i].timestampMs
                  << ",\"rawTimeHex\":\"" << escapeJson(local.records[i].rawTimeHex) << "\"}";
        }
        reply << "],\"ts\":" << nowMs << "}";
        completeListTask(task, reply.str());
        return;
    }

    task.cfgPath = local.files.cfgPath;
    task.cfgBytes = local.files.cfgBytes;
    task.datPath = local.files.datPath;
    task.datBytes = local.files.datBytes;
    task.stage = "local_ready";
    task.nextRetryAtMs = nowMs;
    applyTask(task);
    publishStatus(task, "local_ready", "COMTRADE CFG/DAT files are ready locally");
}

void Iec103RecordingTransferService::processUpload(Task& task, std::int64_t nowMs) {
    if (task.expireAtMs <= nowMs) {
        throw TransferFailure("recording upload token expired before completion", false);
    }
    if (task.cfgPath.empty() || task.datPath.empty()) {
        throw TransferFailure("local recording files are missing from upload task", false);
    }
    task.cfgBytes = fileSize(task.cfgPath);
    task.datBytes = fileSize(task.datPath);
    if (task.cfgSha256.empty()) task.cfgSha256 = sha256File(task.cfgPath);
    if (task.datSha256.empty()) task.datSha256 = sha256File(task.datPath);
    task.stage = "uploading";
    task.nextRetryAtMs = nowMs;
    applyTask(task);
    publishStatus(task, "uploading", "submitting recording manifest");

    const auto manifestFile = temporaryPath(config_.workDirectory, "manifest");
    std::ostringstream manifest;
    manifest << "{\"files\":["
             << "{\"kind\":\"cfg\",\"fileName\":\"" << escapeJson(basenameOf(task.cfgPath))
             << "\",\"sizeBytes\":" << task.cfgBytes << ",\"sha256\":\"" << task.cfgSha256 << "\"},"
             << "{\"kind\":\"dat\",\"fileName\":\"" << escapeJson(basenameOf(task.datPath))
             << "\",\"sizeBytes\":" << task.datBytes << ",\"sha256\":\"" << task.datSha256 << "\"}]}";
    writePrivateFile(manifestFile, manifest.str());
    const std::vector<std::string> jsonHeaders = {
        "Authorization: RecordingUpload " + task.token,
        "Content-Type: application/json"
    };
    try {
        requireHttpSuccess(httpClient_->request("POST", task.baseUrl + "/manifest", jsonHeaders, manifestFile),
            "recording manifest");
    } catch (...) {
        std::remove(manifestFile.c_str());
        throw;
    }
    std::remove(manifestFile.c_str());

    const std::vector<std::string> authHeaders = {"Authorization: RecordingUpload " + task.token};
    const auto partsResponse = httpClient_->request("GET", task.baseUrl + "/parts", authHeaders, "");
    requireHttpSuccess(partsResponse, "recording parts query");
    uploadFileParts(task, "cfg", parsePartNumbers(partsResponse.body, "cfg"));
    uploadFileParts(task, "dat", parsePartNumbers(partsResponse.body, "dat"));

    const auto completeFile = temporaryPath(config_.workDirectory, "complete");
    writePrivateFile(completeFile, "{}");
    publishStatus(task, "verifying", "platform is verifying recording files");
    RecordingHttpResponse completeResponse;
    try {
        completeResponse = httpClient_->request("POST", task.baseUrl + "/complete", jsonHeaders, completeFile);
    } catch (...) {
        std::remove(completeFile.c_str());
        throw;
    }
    std::remove(completeFile.c_str());
    requireHttpSuccess(completeResponse, "recording completion");
    task.stage = "awaiting_ack";
    task.nextRetryAtMs = 0;
    task.lastError.clear();
    (void)applyTask(task);
}

void Iec103RecordingTransferService::uploadFileParts(
    Task& task,
    const std::string& kind,
    const std::vector<int>& received
) {
    const auto path = kind == "cfg" ? task.cfgPath : task.datPath;
    const auto total = kind == "cfg" ? task.cfgBytes : task.datBytes;
    const int partCount = static_cast<int>((total + static_cast<std::uint64_t>(task.partSize) - 1) /
        static_cast<std::uint64_t>(task.partSize));
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) throw std::runtime_error("cannot open recording file for upload: " + path);
    for (int partNo = 0; partNo < partCount; ++partNo) {
        if (containsPart(received, partNo)) continue;
        const auto start = static_cast<std::uint64_t>(partNo) * static_cast<std::uint64_t>(task.partSize);
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(task.partSize, total - start));
        std::string content(bytes, '\0');
        input.seekg(static_cast<std::streamoff>(start));
        input.read(&content[0], static_cast<std::streamsize>(bytes));
        if (static_cast<std::size_t>(input.gcount()) != bytes) {
            throw std::runtime_error("cannot read recording upload part");
        }
        const auto chunkFile = temporaryPath(config_.workDirectory, kind + "-part");
        writePrivateFile(chunkFile, content);
        const auto partSha = sha256File(chunkFile);
        const auto end = start + bytes - 1;
        const std::vector<std::string> headers = {
            "Authorization: RecordingUpload " + task.token,
            "Content-Type: application/octet-stream",
            "Content-Length: " + std::to_string(bytes),
            "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total),
            "X-Part-SHA256: " + partSha
        };
        RecordingHttpResponse response;
        try {
            response = httpClient_->request(
                "PUT",
                task.baseUrl + "/files/" + kind + "/parts/" + std::to_string(partNo),
                headers,
                chunkFile
            );
        } catch (...) {
            std::remove(chunkFile.c_str());
            throw;
        }
        std::remove(chunkFile.c_str());
        requireHttpSuccess(response, "recording " + kind + " part " + std::to_string(partNo));
        publishStatus(task, "uploading",
            "uploaded " + kind + " part " + std::to_string(partNo + 1) + "/" + std::to_string(partCount));
    }
}

bool Iec103RecordingTransferService::applyTask(const Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(task.requestId);
    if (it == tasks_.end() || it->second.stage == "completed") return false;
    it->second = task;
    saveQueueLocked();
    return true;
}

void Iec103RecordingTransferService::completeListTask(const Task& task, const std::string& replyPayload) {
    publisher_->publishJsonMessage(mqttConfig_.recordingReplyTopic, replyPayload);
    publishStatus(task, "completed", "recording directory returned");
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(task.requestId);
    saveQueueLocked();
}

void Iec103RecordingTransferService::scheduleRetry(
    const Task& original,
    const std::string& error,
    std::int64_t nowMs,
    bool retryable
) {
    Task task = original;
    if (task.expireAtMs > 0 && task.expireAtMs <= nowMs) retryable = false;
    task.retryCount += 1;
    task.lastError = error;
    if (!retryable) {
        task.stage = "failed";
        task.nextRetryAtMs = 0;
        if (!applyTask(task)) return;
        publishStatus(task, "failed", error, false, "RECORDING_TRANSFER_FAILED");
        return;
    }
    const int exponent = std::min(task.retryCount - 1, 10);
    const auto delaySec = std::min(config_.retryMaxSec, config_.retryBaseSec * (1 << exponent));
    task.nextRetryAtMs = nowMs + static_cast<std::int64_t>(delaySec) * 1000;
    if (!applyTask(task)) return;
    publishStatus(task, "failed", error, true, "RECORDING_TRANSFER_RETRY");
}

void Iec103RecordingTransferService::publishReply(
    const Task& task,
    const std::string& stage,
    const std::string& message
) const {
    std::ostringstream payload;
    payload << "{\"requestId\":\"" << escapeJson(task.requestId)
            << "\",\"machineCode\":\"" << escapeJson(machineCode_)
            << "\",\"deviceCode\":\"" << escapeJson(task.deviceCode)
            << "\",\"action\":\"" << escapeJson(task.action)
            << "\",\"fan\":" << task.fan
            << ",\"success\":true,\"stage\":\"" << escapeJson(stage)
            << "\",\"message\":\"" << escapeJson(message)
            << "\",\"ts\":" << currentTimeMs() << "}";
    publisher_->publishJsonMessage(mqttConfig_.recordingReplyTopic, payload.str());
}

void Iec103RecordingTransferService::publishStatus(
    const Task& task,
    const std::string& stage,
    const std::string& message,
    bool retryable,
    const std::string& errorCode
) const {
    std::ostringstream payload;
    payload << "{\"requestId\":\"" << escapeJson(task.requestId)
            << "\",\"machineCode\":\"" << escapeJson(machineCode_)
            << "\",\"deviceCode\":\"" << escapeJson(task.deviceCode)
            << "\",\"action\":\"" << escapeJson(task.action)
            << "\",\"fan\":" << task.fan
            << ",\"stage\":\"" << escapeJson(stage)
            << "\",\"retryable\":" << (retryable ? "true" : "false")
            << ",\"retryCount\":" << task.retryCount;
    if (!errorCode.empty()) payload << ",\"errorCode\":\"" << escapeJson(errorCode) << "\"";
    payload << ",\"message\":\"" << escapeJson(message)
            << "\",\"ts\":" << currentTimeMs() << "}";
    publisher_->publishJsonMessage(mqttConfig_.recordingStatusTopic, payload.str());
}

void Iec103RecordingTransferService::loadQueue() {
    std::ifstream input(config_.queueFile.c_str(), std::ios::binary);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.size() > kMaxQueueLineBytes) continue;
        try {
            const auto fields = splitTabs(line);
            if (fields.size() != 21 || fields[0] != "V1") continue;
            Task task;
            task.requestId = percentDecode(fields[1]);
            task.action = percentDecode(fields[2]);
            task.deviceCode = percentDecode(fields[3]);
            task.fan = static_cast<int>(parseInteger(fields[4]));
            task.sessionId = percentDecode(fields[5]);
            task.baseUrl = percentDecode(fields[6]);
            task.token = percentDecode(fields[7]);
            task.expireAtMs = parseInteger(fields[8]);
            task.partSize = static_cast<int>(parseInteger(fields[9]));
            task.socketPath = percentDecode(fields[10]);
            task.stage = percentDecode(fields[11]);
            task.cfgPath = percentDecode(fields[12]);
            task.cfgBytes = static_cast<std::uint64_t>(parseInteger(fields[13]));
            task.cfgSha256 = percentDecode(fields[14]);
            task.datPath = percentDecode(fields[15]);
            task.datBytes = static_cast<std::uint64_t>(parseInteger(fields[16]));
            task.datSha256 = percentDecode(fields[17]);
            task.retryCount = static_cast<int>(parseInteger(fields[18]));
            task.nextRetryAtMs = parseInteger(fields[19]);
            task.lastError = percentDecode(fields[20]);
            if (isSafeId(task.requestId)) tasks_[task.requestId] = std::move(task);
        } catch (...) {
        }
    }
}

void Iec103RecordingTransferService::saveQueueLocked() const {
    const auto temporary = config_.queueFile + ".tmp";
    std::ostringstream content;
    for (const auto& entry : tasks_) {
        const auto& task = entry.second;
        content << "V1"
                << '\t' << percentEncode(task.requestId)
                << '\t' << percentEncode(task.action)
                << '\t' << percentEncode(task.deviceCode)
                << '\t' << task.fan
                << '\t' << percentEncode(task.sessionId)
                << '\t' << percentEncode(task.baseUrl)
                << '\t' << percentEncode(task.token)
                << '\t' << task.expireAtMs
                << '\t' << task.partSize
                << '\t' << percentEncode(task.socketPath)
                << '\t' << percentEncode(task.stage)
                << '\t' << percentEncode(task.cfgPath)
                << '\t' << task.cfgBytes
                << '\t' << percentEncode(task.cfgSha256)
                << '\t' << percentEncode(task.datPath)
                << '\t' << task.datBytes
                << '\t' << percentEncode(task.datSha256)
                << '\t' << task.retryCount
                << '\t' << task.nextRetryAtMs
                << '\t' << percentEncode(task.lastError)
                << '\n';
    }
    writePrivateFile(temporary, content.str());
    replaceFile(temporary, config_.queueFile);
}

void Iec103RecordingTransferService::pruneCompletedLocked() {
    constexpr std::size_t maxCompletedTasks = 128;
    while (static_cast<std::size_t>(std::count_if(tasks_.begin(), tasks_.end(), [](const auto& entry) {
        return entry.second.stage == "completed";
    })) > maxCompletedTasks) {
        auto oldest = tasks_.end();
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
            if (it->second.stage != "completed") continue;
            if (oldest == tasks_.end() || it->second.nextRetryAtMs < oldest->second.nextRetryAtMs) {
                oldest = it;
            }
        }
        if (oldest == tasks_.end()) break;
        tasks_.erase(oldest);
    }
}

}  // namespace edge_gateway
