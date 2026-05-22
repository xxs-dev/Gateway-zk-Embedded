#include "edge_gateway/system_monitor_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::size_t kMaxConfigPullFiles = 256;
constexpr std::size_t kMaxConfigPullFileBytes = 512 * 1024;
constexpr std::size_t kMaxConfigPullTotalBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxConfigPullReplyBytes = 12 * 1024 * 1024;

class FlatJsonReader {
public:
    explicit FlatJsonReader(const std::string& text) : text_(text) {}

    bool tryGetString(const char* key, std::string* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        if (cursor >= text_.size() || text_[cursor] != '"') {
            return false;
        }
        ++cursor;
        std::string value;
        while (cursor < text_.size()) {
            const char ch = text_[cursor++];
            if (ch == '"') {
                *out = value;
                return true;
            }
            if (ch == '\\') {
                if (cursor >= text_.size()) {
                    return false;
                }
                value.push_back(text_[cursor++]);
            } else {
                value.push_back(ch);
            }
        }
        return false;
    }

    bool tryGetInt(const char* key, int* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    }

    bool tryGetInt64(const char* key, std::int64_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        *out = static_cast<std::int64_t>(value);
        return true;
    }

private:
    bool tryGetDouble(const char* key, double* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        const auto begin = cursor;
        if (cursor < text_.size() && (text_[cursor] == '-' || text_[cursor] == '+')) {
            ++cursor;
        }
        while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < text_.size() && text_[cursor] == '.') {
            ++cursor;
            while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
                ++cursor;
            }
        }
        if (begin == cursor) {
            return false;
        }
        *out = std::strtod(text_.c_str() + begin, nullptr);
        return true;
    }

    std::size_t findKey(const char* key) const {
        const std::string needle = std::string("\"") + key + "\"";
        auto keyPos = text_.find(needle);
        if (keyPos == std::string::npos) {
            return std::string::npos;
        }
        keyPos += needle.size();
        keyPos = skipWhitespace(keyPos);
        if (keyPos >= text_.size() || text_[keyPos] != ':') {
            return std::string::npos;
        }
        return keyPos + 1;
    }

    std::size_t skipWhitespace(std::size_t pos) const {
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos])) != 0) {
            ++pos;
        }
        return pos;
    }

    const std::string& text_;
};

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

int countProcesses() {
    int count = 0;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        return 0;
    }
    while (const auto* entry = readdir(dir)) {
        if (entry->d_name == nullptr || entry->d_name[0] == '\0') {
            continue;
        }
        bool numeric = true;
        for (const char* p = entry->d_name; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

std::string readFileText(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::uint64_t fileSizeBytes(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}

std::int64_t modifiedAtMs(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(st.st_mtime) * 1000;
}

std::string extractJsonStringField(const std::string& payload, const char* key) {
    FlatJsonReader json(payload);
    std::string value;
    json.tryGetString(key, &value);
    return value;
}

std::string toHex(const char* data, std::size_t size) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char value = static_cast<unsigned char>(data[i]);
        encoded.push_back(kHex[(value >> 4) & 0x0F]);
        encoded.push_back(kHex[value & 0x0F]);
    }
    return encoded;
}

std::string trimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string truncateText(std::string value, std::size_t maxBytes) {
    if (maxBytes == 0 || value.size() <= maxBytes) {
        return value;
    }
    const std::string suffix = "\n[truncated]";
    if (maxBytes <= suffix.size()) {
        return value.substr(0, maxBytes);
    }
    value.resize(maxBytes - suffix.size());
    value += suffix;
    return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool matchesPattern(const std::string& value, const std::string& pattern) {
    if (pattern.empty()) {
        return false;
    }
    const auto star = pattern.find('*');
    if (star == std::string::npos) {
        return value == pattern;
    }
    const auto prefix = pattern.substr(0, star);
    const auto suffix = pattern.substr(star + 1);
    if (!prefix.empty() && !startsWith(value, prefix)) {
        return false;
    }
    if (!suffix.empty()) {
        return value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return true;
}

std::string baseName(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string dirName(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

bool pathExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool isRegularFile(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string readOptionalFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return trimCopy(buffer.str());
}

std::uint64_t readUintFile(const std::string& path) {
    const auto text = readOptionalFile(path);
    if (text.empty()) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::strtoull(text.c_str(), nullptr, 10));
}

std::vector<std::string> listDirectoryNames(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return names;
    }
    while (const auto* entry = readdir(dir)) {
        if (entry->d_name == nullptr || entry->d_name[0] == '.') {
            continue;
        }
        names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

bool isSafeShellToken(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == '_' || ch == '-' || ch == '.' || ch == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isSafeSystemdUnitName(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    const std::string suffix = ".service";
    if (value.size() <= suffix.size() ||
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == '_' || ch == '-' || ch == '.' || ch == '@' || ch == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string runShellCommand(const std::string& shellCommand, int* exitCode = nullptr) {
    std::string output;
    FILE* pipe = popen(shellCommand.c_str(), "r");
    if (pipe == nullptr) {
        if (exitCode != nullptr) {
            *exitCode = -1;
        }
        return output;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int rc = pclose(pipe);
    if (exitCode != nullptr) {
        *exitCode = rc;
    }
    return output;
}

std::string firstIpv4ForInterface(const std::string& interfaceName) {
    if (!isSafeShellToken(interfaceName)) {
        return std::string();
    }
    const auto output = runShellCommand("ip -4 -o addr show dev " + interfaceName + " 2>/dev/null");
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto inetPos = line.find(" inet ");
        if (inetPos == std::string::npos) {
            continue;
        }
        std::istringstream parts(line.substr(inetPos + 6));
        std::string cidr;
        parts >> cidr;
        const auto slash = cidr.find('/');
        return slash == std::string::npos ? cidr : cidr.substr(0, slash);
    }
    return std::string();
}

std::string routeGatewayForInterface(const std::string& interfaceName) {
    std::ifstream route("/proc/net/route");
    std::string line;
    std::getline(route, line);
    while (std::getline(route, line)) {
        std::istringstream parts(line);
        std::string iface;
        std::string destination;
        std::string gatewayHex;
        parts >> iface >> destination >> gatewayHex;
        if (iface != interfaceName || destination != "00000000" || gatewayHex.size() != 8) {
            continue;
        }
        unsigned long raw = std::strtoul(gatewayHex.c_str(), nullptr, 16);
        std::ostringstream gateway;
        gateway << (raw & 0xFF) << "."
                << ((raw >> 8) & 0xFF) << "."
                << ((raw >> 16) & 0xFF) << "."
                << ((raw >> 24) & 0xFF);
        return gateway.str();
    }
    return std::string();
}

std::string dnsServers() {
    std::ifstream input("/etc/resolv.conf");
    std::string line;
    std::vector<std::string> servers;
    while (std::getline(input, line)) {
        line = trimCopy(line);
        if (!startsWith(line, "nameserver")) {
            continue;
        }
        std::istringstream parts(line);
        std::string label;
        std::string value;
        parts >> label >> value;
        if (!value.empty()) {
            servers.push_back(value);
        }
    }
    std::ostringstream joined;
    for (std::size_t i = 0; i < servers.size(); ++i) {
        if (i > 0) {
            joined << ",";
        }
        joined << servers[i];
    }
    return joined.str();
}

std::vector<std::string> discoverModemDevices(const std::vector<std::string>& patterns) {
    std::vector<std::string> devices;
    for (const auto& pattern : patterns) {
        if (pattern.find('*') == std::string::npos) {
            if (pathExists(pattern) && std::find(devices.begin(), devices.end(), pattern) == devices.end()) {
                devices.push_back(pattern);
            }
            continue;
        }
        const auto dir = dirName(pattern);
        const auto namePattern = baseName(pattern);
        for (const auto& name : listDirectoryNames(dir)) {
            if (!matchesPattern(name, namePattern)) {
                continue;
            }
            const auto full = dir + "/" + name;
            if (std::find(devices.begin(), devices.end(), full) == devices.end()) {
                devices.push_back(full);
            }
        }
    }
    return devices;
}

std::string maskSensitive(std::string value) {
    if (value.size() <= 6) {
        return value.empty() ? value : "***";
    }
    return value.substr(0, 2) + std::string(value.size() - 4, '*') + value.substr(value.size() - 2);
}

std::string stripQuotes(std::string value) {
    value = trimCopy(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<std::string> splitCsvLine(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    bool quoted = false;
    for (const auto ch : value) {
        if (ch == '"') {
            quoted = !quoted;
            current.push_back(ch);
        } else if (ch == ',' && !quoted) {
            parts.push_back(trimCopy(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(trimCopy(current));
    return parts;
}

std::string valueAfterColon(const std::string& line) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) {
        return std::string();
    }
    return trimCopy(line.substr(pos + 1));
}

double parseFirstNumber(const std::string& text, double fallback) {
    const char* begin = text.c_str();
    while (*begin != '\0') {
        if ((*begin >= '0' && *begin <= '9') || *begin == '-' || *begin == '+') {
            char* end = nullptr;
            const double value = std::strtod(begin, &end);
            if (end != begin) {
                return value;
            }
        }
        ++begin;
    }
    return fallback;
}

#ifndef _WIN32
speed_t baudToTermiosSpeed(int baudRate) {
    switch (baudRate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
    }
}
#endif

std::string atCommand(const std::string& device, int baudRate, const std::string& command, int timeoutMs) {
#ifdef _WIN32
    (void)device;
    (void)baudRate;
    (void)command;
    (void)timeoutMs;
    return std::string();
#else
    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return std::string();
    }
    termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        ::close(fd);
        return std::string();
    }
    cfmakeraw(&tty);
    const auto baud = baudToTermiosSpeed(baudRate);
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        ::close(fd);
        return std::string();
    }
    tcflush(fd, TCIOFLUSH);

    const std::string request = command + "\r";
    const auto written = ::write(fd, request.data(), request.size());
    if (written < 0 || static_cast<std::size_t>(written) != request.size()) {
        ::close(fd);
        return std::string();
    }
    tcdrain(fd);

    std::string response;
    const auto deadline = currentTimeMs() + std::max(200, timeoutMs);
    while (currentTimeMs() < deadline) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        const auto ready = select(fd + 1, &readSet, nullptr, nullptr, &tv);
        if (ready > 0) {
            char buffer[256];
            const auto rc = ::read(fd, buffer, sizeof(buffer));
            if (rc > 0) {
                response.append(buffer, buffer + rc);
                if (response.find("\r\nOK") != std::string::npos ||
                    response.find("\nOK") != std::string::npos ||
                    response.find("ERROR") != std::string::npos) {
                    break;
                }
            }
        } else if (ready < 0) {
            break;
        }
    }
    ::close(fd);
    return response;
#endif
}

std::string atPayloadLine(const std::string& response, const std::string& prefix = std::string()) {
    std::istringstream lines(response);
    std::string line;
    while (std::getline(lines, line)) {
        line = trimCopy(line);
        if (line.empty() || line == "OK" || line == "ERROR" || startsWith(line, "AT")) {
            continue;
        }
        if (!prefix.empty()) {
            if (startsWith(line, prefix)) {
                return line;
            }
            continue;
        }
        return line;
    }
    return std::string();
}

std::string accessTechFromAct(int act) {
    switch (act) {
        case 0: return "GSM";
        case 2: return "UTRAN";
        case 3: return "EDGE";
        case 4: return "HSDPA";
        case 5: return "HSUPA";
        case 6: return "HSPA";
        case 7: return "LTE";
        case 8: return "EC-GSM-IoT";
        case 9: return "NB-IoT";
        case 10: return "NR";
        default: return std::string();
    }
}

template <typename CellularStatus>
bool applyAtOutput(
    const std::string& device,
    int baudRate,
    int timeoutMs,
    CellularStatus* status
) {
    const auto at = atCommand(device, baudRate, "AT", timeoutMs);
    if (at.empty() || at.find("OK") == std::string::npos) {
        return false;
    }
    status->toolsAvailable = true;
    status->present = true;
    status->modemDevice = device;

    const auto cpin = atPayloadLine(atCommand(device, baudRate, "AT+CPIN?", timeoutMs), "+CPIN:");
    if (!cpin.empty()) {
        status->simStatus = valueAfterColon(cpin);
        status->simReady = status->simStatus.find("READY") != std::string::npos ||
            status->simStatus.find("ready") != std::string::npos;
    }

    const auto csq = atPayloadLine(atCommand(device, baudRate, "AT+CSQ", timeoutMs), "+CSQ:");
    if (!csq.empty()) {
        const auto parts = splitCsvLine(valueAfterColon(csq));
        if (!parts.empty()) {
            const int rssi = std::atoi(parts.front().c_str());
            if (rssi >= 0 && rssi <= 31) {
                status->signalPercent = static_cast<double>(rssi) * 100.0 / 31.0;
                status->rssiDbm = -113.0 + 2.0 * static_cast<double>(rssi);
            }
        }
    }

    const auto cops = atPayloadLine(atCommand(device, baudRate, "AT+COPS?", timeoutMs), "+COPS:");
    if (!cops.empty()) {
        const auto parts = splitCsvLine(valueAfterColon(cops));
        if (parts.size() >= 3) {
            status->operatorName = stripQuotes(parts[2]);
        }
        if (parts.size() >= 4) {
            status->accessTech = accessTechFromAct(std::atoi(parts[3].c_str()));
        }
    }

    const auto creg = atPayloadLine(atCommand(device, baudRate, "AT+CREG?", timeoutMs), "+CREG:");
    if (!creg.empty()) {
        const auto parts = splitCsvLine(valueAfterColon(creg));
        const int stat = parts.size() >= 2 ? std::atoi(parts[1].c_str()) : (parts.empty() ? 0 : std::atoi(parts[0].c_str()));
        if (stat == 1 || stat == 5) {
            status->registered = true;
        }
    }

    const auto cereg = atPayloadLine(atCommand(device, baudRate, "AT+CEREG?", timeoutMs), "+CEREG:");
    if (!cereg.empty()) {
        const auto parts = splitCsvLine(valueAfterColon(cereg));
        const int stat = parts.size() >= 2 ? std::atoi(parts[1].c_str()) : (parts.empty() ? 0 : std::atoi(parts[0].c_str()));
        if (stat == 1 || stat == 5) {
            status->registered = true;
            if (status->accessTech.empty()) {
                status->accessTech = "LTE";
            }
        }
    }

    const auto cgatt = atPayloadLine(atCommand(device, baudRate, "AT+CGATT?", timeoutMs), "+CGATT:");
    if (!cgatt.empty() && std::atoi(valueAfterColon(cgatt).c_str()) == 1) {
        status->connected = true;
        status->registered = true;
    }

    const auto cgpaddr = atPayloadLine(atCommand(device, baudRate, "AT+CGPADDR", timeoutMs), "+CGPADDR:");
    if (!cgpaddr.empty() && status->ipAddress.empty()) {
        const auto parts = splitCsvLine(valueAfterColon(cgpaddr));
        for (const auto& part : parts) {
            const auto item = stripQuotes(part);
            if (item.find('.') != std::string::npos) {
                status->ipAddress = item;
                status->connected = true;
                break;
            }
        }
    }

    const auto imei = atPayloadLine(atCommand(device, baudRate, "AT+CGSN", timeoutMs));
    if (!imei.empty()) {
        status->imei = imei;
    }
    const auto ccid = atPayloadLine(atCommand(device, baudRate, "AT+CCID", timeoutMs), "+CCID:");
    if (!ccid.empty()) {
        status->iccid = valueAfterColon(ccid);
    }
    return true;
}

template <typename CellularStatus>
void applyMmcliOutput(const std::string& output, CellularStatus* status) {
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        line = trimCopy(line);
        if (line.empty()) {
            continue;
        }
        const auto value = valueAfterColon(line);
        if (line.find("operator-name") != std::string::npos) {
            status->operatorName = value;
        } else if (line.find("access-technologies") != std::string::npos && !value.empty()) {
            status->accessTech = status->accessTech.empty() ? value : status->accessTech + "," + value;
        } else if (line.find("signal-quality") != std::string::npos) {
            status->signalPercent = parseFirstNumber(value, status->signalPercent);
        } else if (line.find("equipment-identifier") != std::string::npos) {
            status->imei = value;
        } else if (line.find("primary-port") != std::string::npos && status->modemDevice.empty()) {
            status->modemDevice = "/dev/" + value;
        } else if (line.find("state") != std::string::npos) {
            const auto lower = value;
            if (lower.find("connected") != std::string::npos) {
                status->connected = true;
                status->registered = true;
            } else if (lower.find("registered") != std::string::npos || lower.find("enabled") != std::string::npos) {
                status->registered = true;
            }
        } else if (line.find("registration-state") != std::string::npos) {
            if (value.find("home") != std::string::npos || value.find("roaming") != std::string::npos ||
                value.find("registered") != std::string::npos) {
                status->registered = true;
            }
        } else if (line.find("sim") != std::string::npos && line.find("state") != std::string::npos) {
            status->simStatus = value;
            status->simReady = value.find("ready") != std::string::npos ||
                value.find("enabled") != std::string::npos ||
                value.find("available") != std::string::npos;
        } else if (line.find("iccid") != std::string::npos) {
            status->iccid = value;
        } else if (line.find("imsi") != std::string::npos) {
            status->imsi = value;
        }
    }
}

}  // namespace

SystemMonitorService::SystemMonitorService(
    SystemMonitorConfig monitorConfig,
    MqttConfig mqttConfig,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::string machineCode,
    std::vector<std::string> configFiles,
    PointStoreRouter* router
) : monitorConfig_(std::move(monitorConfig)),
    mqttConfig_(std::move(mqttConfig)),
    publisher_(std::move(publisher)),
    machineCode_(std::move(machineCode)),
    configFiles_(std::move(configFiles)),
    router_(router) {
    if (!publisher_) {
        throw std::invalid_argument("system monitor publisher is required");
    }
}

SystemMonitorService::~SystemMonitorService() {
    stop();
}

void SystemMonitorService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    publishStatusEvent("started", currentTimeMs());
    thread_ = std::thread(&SystemMonitorService::loop, this);
}

void SystemMonitorService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SystemMonitorService::isRunning() const {
    return running_.load();
}

void SystemMonitorService::runOnce(std::int64_t nowMs) {
    processIncomingMessages(nowMs);
    const auto sample = collectSample();
    if (hasActiveLease(nowMs) && (lastTelemetryMs_ == 0 || nowMs - lastTelemetryMs_ >= effectiveIntervalMs(nowMs))) {
        publishTelemetry(sample, nowMs);
        publishPointSnapshot(nowMs);
        lastTelemetryMs_ = nowMs;
    }
    evaluateAlerts(sample, nowMs);
}

void SystemMonitorService::loop() {
    while (running_.load()) {
        runOnce(currentTimeMs());
        sleepInterruptibly(running_, 500);
    }
    publishStatusEvent("stopped", currentTimeMs());
}

void SystemMonitorService::processIncomingMessages(std::int64_t nowMs) {
    std::vector<MqttIncomingMessage> messages;
    try {
        messages = publisher_->pollIncoming(100);
    } catch (const std::exception& ex) {
        publishStatusEvent("poll-failed", nowMs, std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
        return;
    }
    for (const auto& message : messages) {
        try {
            if (message.type == MqttIncomingType::SystemMonitorRequest) {
                handleMonitorRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::DiagRequest) {
                handleDiagRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::ConfigPullRequest) {
                handleConfigPullRequest(message.payload, nowMs);
            }
        } catch (const std::exception& ex) {
            publishStatusEvent("request-failed", nowMs, std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
        }
    }
}

void SystemMonitorService::handleMonitorRequest(const std::string& payload, std::int64_t nowMs) {
    FlatJsonReader json(payload);
    std::string action;
    std::string sessionId;
    std::string machineCode;
    int intervalMs = monitorConfig_.defaultIntervalMs;
    int ttlSec = monitorConfig_.subscriptionTtlSec;
    json.tryGetString("action", &action);
    json.tryGetString("sessionId", &sessionId);
    json.tryGetString("machineCode", &machineCode);
    json.tryGetInt("intervalMs", &intervalMs);
    json.tryGetInt("ttlSec", &ttlSec);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (sessionId.empty()) {
        sessionId = "system-monitor";
    }
    if (action == "unsubscribe") {
        leases_.erase(sessionId);
    } else {
        MonitorLease lease;
        lease.sessionId = sessionId;
        lease.intervalMs = std::max(monitorConfig_.minIntervalMs, intervalMs);
        lease.expireAtMs = nowMs + static_cast<std::int64_t>(std::max(1, ttlSec)) * 1000;
        leases_[sessionId] = lease;
        publishPointSnapshot(nowMs);
    }
    publishStatusEvent(
        "monitor-request",
        nowMs,
        std::string(R"("sessionId":")") + escapeJson(sessionId) +
            R"(","action":")" + escapeJson(action.empty() ? "subscribe" : action) +
            R"(")"
    );
    std::ostringstream reply;
    reply << "{\"machineCode\":\"" << escapeJson(machineCode_) << "\",\"sessionId\":\"" << escapeJson(sessionId)
          << "\",\"action\":\"" << escapeJson(action.empty() ? "subscribe" : action)
          << "\",\"accepted\":true,\"ts\":" << nowMs << "}";
    publishReply(reply.str());
}

void SystemMonitorService::handleDiagRequest(const std::string& payload, std::int64_t nowMs) {
    if (!monitorConfig_.diagEnabled) {
        throw std::runtime_error("diag disabled");
    }
    FlatJsonReader json(payload);
    std::string cmdId;
    std::string command;
    std::string machineCode;
    std::string operatorName;
    std::string service;
    int tailLines = 100;
    std::int64_t requestTs = nowMs;
    json.tryGetString("cmdId", &cmdId);
    json.tryGetString("command", &command);
    json.tryGetString("machineCode", &machineCode);
    json.tryGetString("operator", &operatorName);
    json.tryGetString("service", &service);
    json.tryGetInt("tailLines", &tailLines);
    json.tryGetInt64("requestTs", &requestTs);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (cmdId.empty()) {
        cmdId = "DIAG_" + std::to_string(nowMs);
    }
    publishStatusEvent(
        "diag-request",
        nowMs,
        std::string(R"("cmdId":")") + escapeJson(cmdId) +
            R"(","command":")" + escapeJson(command) +
            R"(")"
    );
    if (!isCommandAllowed(command)) {
        throw std::runtime_error("diag command not allowed");
    }
    int exitCode = 0;
    std::string arg;
    if (command == "journal_tail") {
        arg = std::to_string(std::max(1, std::min(500, tailLines)));
    } else if (command == "systemctl_status") {
        arg = service;
    }
    const auto stdoutText = truncateText(
        executeDiagCommand(command, arg, &exitCode),
        monitorConfig_.maxDiagOutputBytes
    );
    std::ostringstream reply;
    reply << "{\"cmdId\":\"" << escapeJson(cmdId)
          << "\",\"machineCode\":\"" << escapeJson(machineCode)
          << "\",\"operator\":\"" << escapeJson(operatorName)
          << "\",\"command\":\"" << escapeJson(command)
          << "\",\"success\":" << (exitCode == 0 ? "true" : "false")
          << ",\"exitCode\":" << exitCode
          << ",\"stdout\":\"" << escapeJson(stdoutText)
          << "\",\"stderr\":\"\""
          << ",\"requestTs\":" << requestTs
          << ",\"ts\":" << nowMs
          << "}";
    publishReply(reply.str());
    publishStatusEvent(
        "diag-replied",
        nowMs,
        std::string(R"("cmdId":")") + escapeJson(cmdId) +
            R"(","exitCode":)" + std::to_string(exitCode)
    );
}

void SystemMonitorService::handleConfigPullRequest(const std::string& payload, std::int64_t nowMs) {
    FlatJsonReader json(payload);
    std::string requestId;
    std::string machineCode;
    json.tryGetString("requestId", &requestId);
    json.tryGetString("machineCode", &machineCode);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (requestId.empty()) {
        requestId = "CFG_PULL_" + std::to_string(nowMs);
    }

    const auto reply = buildConfigPullReply(requestId, nowMs);
    publishConfigPullReply(reply);
    publishStatusEvent(
        "config-pull-replied",
        nowMs,
        std::string(R"("requestId":")") + escapeJson(requestId) +
            R"(","fileCount":)" + std::to_string(configFiles_.size())
    );
}

std::string SystemMonitorService::buildConfigPullReply(const std::string& requestId, std::int64_t nowMs) const {
    std::ostringstream reply;
    reply << "{\"requestId\":\"" << escapeJson(requestId)
          << "\",\"machineCode\":\"" << escapeJson(machineCode_)
          << "\",\"success\":true,\"files\":[";

    std::size_t emittedFiles = 0;
    std::size_t skippedFiles = 0;
    std::size_t totalBytes = 0;
    for (const auto& path : configFiles_) {
        if (path.empty()) {
            continue;
        }
        if (emittedFiles >= kMaxConfigPullFiles) {
            ++skippedFiles;
            continue;
        }
        if (!isRegularFile(path)) {
            ++skippedFiles;
            continue;
        }
        const auto size = static_cast<std::size_t>(fileSizeBytes(path));
        if (size > kMaxConfigPullFileBytes || totalBytes > kMaxConfigPullTotalBytes - size) {
            ++skippedFiles;
            continue;
        }

        const auto content = readFileText(path);
        if (content.size() > kMaxConfigPullFileBytes ||
            totalBytes > kMaxConfigPullTotalBytes - content.size()) {
            ++skippedFiles;
            continue;
        }
        totalBytes += content.size();
        if (emittedFiles > 0) {
            reply << ",";
        }
        reply << "{\"path\":\"" << escapeJson(path)
              << "\",\"sizeBytes\":" << static_cast<long long>(content.size())
              << ",\"modifiedAtMs\":" << static_cast<long long>(modifiedAtMs(path))
              << ",\"content\":\"" << escapeJson(content) << "\"}";
        ++emittedFiles;
        if (reply.tellp() > static_cast<std::streampos>(kMaxConfigPullReplyBytes)) {
            throw std::runtime_error("config pull reply is too large");
        }
    }
    reply << "],\"fileCount\":" << static_cast<long long>(emittedFiles)
          << ",\"skippedFiles\":" << static_cast<long long>(skippedFiles)
          << ",\"totalBytes\":" << static_cast<long long>(totalBytes)
          << ",\"ts\":" << nowMs << "}";
    if (reply.tellp() > static_cast<std::streampos>(kMaxConfigPullReplyBytes)) {
        throw std::runtime_error("config pull reply is too large");
    }
    return reply.str();
}

SystemMonitorService::Sample SystemMonitorService::collectSample() const {
    Sample sample;

    std::ifstream stat("/proc/stat");
    std::string cpuLabel;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (stat >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
        const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
        const auto idleTotal = idle + iowait;
        if (cpuBaselineReady_ && total > lastCpuTotal_) {
            const auto totalDiff = total - lastCpuTotal_;
            const auto idleDiff = idleTotal - lastCpuIdle_;
            if (totalDiff > 0) {
                sample.cpuUsage = 100.0 * static_cast<double>(totalDiff - idleDiff) / static_cast<double>(totalDiff);
            }
        }
        const_cast<SystemMonitorService*>(this)->lastCpuTotal_ = total;
        const_cast<SystemMonitorService*>(this)->lastCpuIdle_ = idleTotal;
        const_cast<SystemMonitorService*>(this)->cpuBaselineReady_ = true;
    }

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t totalMem = 0, availMem = 0;
    while (meminfo >> key) {
        std::uint64_t value = 0;
        meminfo >> value;
        if (key == "MemTotal:") {
            totalMem = value;
        } else if (key == "MemAvailable:") {
            availMem = value;
        }
        meminfo.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (totalMem > 0 && availMem <= totalMem) {
        sample.memUsage = 100.0 * static_cast<double>(totalMem - availMem) / static_cast<double>(totalMem);
    }

    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        const auto used = vfs.f_blocks - vfs.f_bavail;
        sample.diskUsage = 100.0 * static_cast<double>(used) / static_cast<double>(vfs.f_blocks);
    }

    std::ifstream loadavg("/proc/loadavg");
    if (loadavg) {
        loadavg >> sample.load1;
    }
    sample.processCount = countProcesses();
    sample.cellular = collectCellularStatus(currentTimeMs());
    return sample;
}

SystemMonitorService::Sample::CellularStatus SystemMonitorService::collectCellularStatus(std::int64_t nowMs) const {
    Sample::CellularStatus status;
    status.enabled = monitorConfig_.cellular.enabled;
    status.ts = nowMs;
    if (!monitorConfig_.cellular.enabled) {
        lastCellularStatus_ = status;
        lastCellularProbeMs_ = nowMs;
        return status;
    }

    const int probeIntervalMs = std::max(1000, monitorConfig_.cellular.probeIntervalMs);
    if (lastCellularProbeMs_ > 0 && nowMs - lastCellularProbeMs_ < probeIntervalMs) {
        status = lastCellularStatus_;
        status.ts = nowMs;
        return status;
    }

    try {
        const auto modemDevices = discoverModemDevices(monitorConfig_.cellular.modemDevicePatterns);
        if (!modemDevices.empty()) {
            status.present = true;
            status.modemDevice = modemDevices.front();
        }

        for (const auto& name : listDirectoryNames("/sys/class/net")) {
            if (name == "lo") {
                continue;
            }
            bool matched = false;
            for (const auto& pattern : monitorConfig_.cellular.interfacePatterns) {
                if (matchesPattern(name, pattern)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                continue;
            }

            const auto base = "/sys/class/net/" + name;
            const auto ip = firstIpv4ForInterface(name);
            const auto operState = readOptionalFile(base + "/operstate");
            const auto carrier = readOptionalFile(base + "/carrier");
            const auto rxBytes = readUintFile(base + "/statistics/rx_bytes");
            const auto txBytes = readUintFile(base + "/statistics/tx_bytes");
            status.present = true;
            if (status.interfaceName.empty() || !ip.empty()) {
                const auto previousRx = status.rxBytes;
                const auto previousTx = status.txBytes;
                (void)previousRx;
                (void)previousTx;
                status.interfaceName = name;
                status.ipAddress = ip;
                status.gateway = routeGatewayForInterface(name);
                status.dns = dnsServers();
                status.rxBytes = rxBytes;
                status.txBytes = txBytes;
                status.connected = !ip.empty() && (operState == "up" || operState == "unknown" || carrier == "1");
                if (status.connected) {
                    status.registered = true;
                }
            }
            if (!status.ipAddress.empty()) {
                break;
            }
        }

        if (!status.interfaceName.empty() && lastCellularStatus_.interfaceName == status.interfaceName &&
            lastCellularStatus_.ts > 0 && nowMs > lastCellularStatus_.ts) {
            const auto deltaMs = nowMs - lastCellularStatus_.ts;
            if (status.rxBytes >= lastCellularStatus_.rxBytes) {
                status.rxRateBps = static_cast<double>(status.rxBytes - lastCellularStatus_.rxBytes) * 1000.0 /
                    static_cast<double>(deltaMs);
            }
            if (status.txBytes >= lastCellularStatus_.txBytes) {
                status.txRateBps = static_cast<double>(status.txBytes - lastCellularStatus_.txBytes) * 1000.0 /
                    static_cast<double>(deltaMs);
            }
        }

        int mmcliExit = 0;
        const auto mmcli = runShellCommand(
            "sh -c 'command -v mmcli >/dev/null 2>&1 && mmcli -m 0 --output-keyvalue 2>/dev/null'",
            &mmcliExit
        );
        if (!mmcli.empty()) {
            status.toolsAvailable = true;
            status.present = true;
            applyMmcliOutput(mmcli, &status);
        }

        for (const auto& modemDevice : modemDevices) {
            if (applyAtOutput(
                    modemDevice,
                    monitorConfig_.cellular.atBaudRate,
                    monitorConfig_.cellular.commandTimeoutMs,
                    &status
                )) {
                break;
            }
        }

        if (status.simStatus.empty() && status.present) {
            status.simStatus = "unknown";
        }
        if (status.present && status.registered && status.signalPercent < 0.0) {
            status.signalPercent = 0.0;
        }
        if (monitorConfig_.cellular.maskSensitiveFields) {
            status.imei = maskSensitive(status.imei);
            status.imsi = maskSensitive(status.imsi);
            status.iccid = maskSensitive(status.iccid);
        }
        if (!status.present) {
            status.lastError = "cellular modem not found";
        } else if (!status.connected) {
            status.lastError = "cellular network not connected";
        }
    } catch (const std::exception& ex) {
        status.lastError = ex.what();
    }

    lastCellularStatus_ = status;
    lastCellularProbeMs_ = nowMs;
    return status;
}

void SystemMonitorService::publishTelemetry(const Sample& sample, std::int64_t nowMs) {
    if (mqttConfig_.systemMonitorTelemetryTopic.empty()) {
        return;
    }
    const auto& cellular = sample.cellular;
    std::ostringstream payload;
    payload << "{\"type\":\"system-monitor\",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"cpuUsage\":" << sample.cpuUsage
            << ",\"memUsage\":" << sample.memUsage
            << ",\"diskUsage\":" << sample.diskUsage
            << ",\"load1\":" << sample.load1
            << ",\"processCount\":" << sample.processCount
            << ",\"cellular\":{"
            << "\"enabled\":" << (cellular.enabled ? "true" : "false")
            << ",\"present\":" << (cellular.present ? "true" : "false")
            << ",\"registered\":" << (cellular.registered ? "true" : "false")
            << ",\"connected\":" << (cellular.connected ? "true" : "false")
            << ",\"simReady\":" << (cellular.simReady ? "true" : "false")
            << ",\"toolsAvailable\":" << (cellular.toolsAvailable ? "true" : "false")
            << ",\"operator\":\"" << escapeJson(cellular.operatorName) << "\""
            << ",\"accessTech\":\"" << escapeJson(cellular.accessTech) << "\""
            << ",\"interfaceName\":\"" << escapeJson(cellular.interfaceName) << "\""
            << ",\"ipAddress\":\"" << escapeJson(cellular.ipAddress) << "\""
            << ",\"gateway\":\"" << escapeJson(cellular.gateway) << "\""
            << ",\"dns\":\"" << escapeJson(cellular.dns) << "\""
            << ",\"simStatus\":\"" << escapeJson(cellular.simStatus) << "\""
            << ",\"imei\":\"" << escapeJson(cellular.imei) << "\""
            << ",\"imsi\":\"" << escapeJson(cellular.imsi) << "\""
            << ",\"iccid\":\"" << escapeJson(cellular.iccid) << "\""
            << ",\"modemDevice\":\"" << escapeJson(cellular.modemDevice) << "\""
            << ",\"lastError\":\"" << escapeJson(cellular.lastError) << "\""
            << ",\"signalPercent\":" << cellular.signalPercent
            << ",\"rssiDbm\":" << cellular.rssiDbm
            << ",\"rsrpDbm\":" << cellular.rsrpDbm
            << ",\"rsrqDb\":" << cellular.rsrqDb
            << ",\"sinrDb\":" << cellular.sinrDb
            << ",\"rxBytes\":" << static_cast<unsigned long long>(cellular.rxBytes)
            << ",\"txBytes\":" << static_cast<unsigned long long>(cellular.txBytes)
            << ",\"rxRateBps\":" << cellular.rxRateBps
            << ",\"txRateBps\":" << cellular.txRateBps
            << ",\"ts\":" << cellular.ts
            << "}"
            << ",\"ts\":" << nowMs
            << "}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorTelemetryTopic, payload.str());
}

void SystemMonitorService::evaluateAlerts(const Sample& sample, std::int64_t nowMs) {
    if (sample.cpuUsage >= monitorConfig_.cpuAlertThreshold) {
        publishAlert("cpuUsage", sample.cpuUsage, monitorConfig_.cpuAlertThreshold, true, nowMs, "cpu usage too high");
    }
    if (sample.memUsage >= monitorConfig_.memAlertThreshold) {
        publishAlert("memUsage", sample.memUsage, monitorConfig_.memAlertThreshold, true, nowMs, "memory usage too high");
    }
    if (sample.diskUsage >= monitorConfig_.diskAlertThreshold) {
        publishAlert("diskUsage", sample.diskUsage, monitorConfig_.diskAlertThreshold, true, nowMs, "disk usage too high");
    }
    if (monitorConfig_.cellular.enabled) {
        if (!sample.cellular.present) {
            publishAlert("cellularPresent", 0.0, 1.0, true, nowMs, "cellular modem not found");
        } else if (!sample.cellular.connected) {
            publishAlert("cellularConnected", 0.0, 1.0, true, nowMs, "cellular network not connected");
        }
        if (sample.cellular.signalPercent >= 0.0 &&
            sample.cellular.signalPercent < monitorConfig_.cellular.signalAlertThresholdPercent) {
            publishAlert(
                "cellularSignalPercent",
                sample.cellular.signalPercent,
                monitorConfig_.cellular.signalAlertThresholdPercent,
                true,
                nowMs,
                "cellular signal too weak"
            );
        }
    }
}

void SystemMonitorService::publishAlert(
    const std::string& metric,
    double value,
    double threshold,
    bool active,
    std::int64_t nowMs,
    const std::string& message
) {
    if (mqttConfig_.systemMonitorAlertTopic.empty()) {
        return;
    }
    const auto it = lastAlertPublishMs_.find(metric);
    const auto minInterval = static_cast<std::int64_t>(std::max(1, monitorConfig_.alertRepeatIntervalSec)) * 1000;
    if (it != lastAlertPublishMs_.end() && nowMs - it->second < minInterval) {
        return;
    }
    lastAlertPublishMs_[metric] = nowMs;
    std::ostringstream payload;
    payload << "{\"type\":\"system-alert\",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"metric\":\"" << escapeJson(metric) << "\""
            << ",\"level\":\"critical\""
            << ",\"value\":" << value
            << ",\"threshold\":" << threshold
            << ",\"active\":" << (active ? "true" : "false")
            << ",\"message\":\"" << escapeJson(message) << "\""
            << ",\"ts\":" << nowMs
            << "}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorAlertTopic, payload.str());
}

void SystemMonitorService::publishReply(const std::string& payload) {
    if (payload.find("\"cmdId\"") != std::string::npos) {
        publisher_->publishJsonMessage(mqttConfig_.diagReplyTopic, payload);
    } else {
        publisher_->publishJsonMessage(mqttConfig_.systemMonitorReplyTopic, payload);
    }
}

void SystemMonitorService::publishConfigPullReply(const std::string& payload) const {
    static const std::size_t kConfigPullChunkBytes = 16 * 1024;
    if (payload.size() <= kConfigPullChunkBytes) {
        publisher_->publishJsonMessage(mqttConfig_.configPullReplyTopic, payload);
        return;
    }

    const std::string requestId = extractJsonStringField(payload, "requestId");
    const std::size_t chunkCount = (payload.size() + kConfigPullChunkBytes - 1) / kConfigPullChunkBytes;
    for (std::size_t i = 0; i < chunkCount; ++i) {
        const std::size_t begin = i * kConfigPullChunkBytes;
        const std::size_t partSize = std::min(kConfigPullChunkBytes, payload.size() - begin);
        const std::string partHex = toHex(payload.data() + begin, partSize);
        std::ostringstream chunk;
        chunk << "{\"requestId\":\"" << escapeJson(requestId)
              << "\",\"machineCode\":\"" << escapeJson(machineCode_)
              << "\",\"success\":true"
              << ",\"chunked\":true"
              << ",\"chunkIndex\":" << static_cast<long long>(i + 1)
              << ",\"chunkCount\":" << static_cast<long long>(chunkCount)
              << ",\"totalBytes\":" << static_cast<long long>(payload.size())
              << ",\"payloadHex\":\"" << partHex << "\""
              << ",\"ts\":" << currentTimeMs()
              << "}";
        publisher_->publishJsonMessage(mqttConfig_.configPullReplyTopic, chunk.str());
    }
}

void SystemMonitorService::publishPointSnapshot(std::int64_t nowMs) {
    if (router_ == nullptr || mqttConfig_.systemMonitorPointTopic.empty()) {
        return;
    }
    const auto values = router_->getAllLatest(nowMs);
    if (values.empty()) {
        return;
    }
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& value : values) {
        std::ostringstream item;
        item << "{\"index\":" << value.index
             << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
             << ",\"value\":" << value.value
             << ",\"quality\":" << value.quality
             << ",\"ts\":" << value.ts
             << ",\"stale\":" << (value.stale ? "true" : "false")
             << "}";
        grouped[value.meterCode].push_back(item.str());
    }
    std::ostringstream payload;
    payload << "{\"type\":\"snapshot\",\"machineCode\":\"" << escapeJson(machineCode_) << "\",\"meters\":[";
    bool firstMeter = true;
    for (const auto& entry : grouped) {
        if (!firstMeter) {
            payload << ",";
        }
        firstMeter = false;
        payload << "{\"meterCode\":\"" << escapeJson(entry.first) << "\",\"values\":[";
        for (std::size_t i = 0; i < entry.second.size(); ++i) {
            if (i > 0) {
                payload << ",";
            }
            payload << entry.second[i];
        }
        payload << "]}";
    }
    payload << "]}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorPointTopic, payload.str());
}

void SystemMonitorService::publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson) const {
    if (mqttConfig_.statusTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"system-monitor\",\"event\":\"" << escapeJson(event) << "\",\"ts\":" << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
}

bool SystemMonitorService::hasActiveLease(std::int64_t nowMs) const {
    for (const auto& entry : leases_) {
        if (entry.second.expireAtMs > nowMs) {
            return true;
        }
    }
    return false;
}

int SystemMonitorService::effectiveIntervalMs(std::int64_t nowMs) const {
    int interval = monitorConfig_.defaultIntervalMs;
    for (const auto& entry : leases_) {
        if (entry.second.expireAtMs <= nowMs) {
            continue;
        }
        interval = std::min(interval, entry.second.intervalMs);
    }
    return std::max(monitorConfig_.minIntervalMs, interval);
}

bool SystemMonitorService::isCommandAllowed(const std::string& command) const {
    return std::find(
        monitorConfig_.allowedCommands.begin(),
        monitorConfig_.allowedCommands.end(),
        command
    ) != monitorConfig_.allowedCommands.end();
}

std::string SystemMonitorService::executeDiagCommand(const std::string& command, const std::string& arg, int* exitCode) const {
    std::string shellCommand;
    if (command == "uptime") {
        shellCommand = "uptime 2>&1";
    } else if (command == "free_m") {
        shellCommand = "free -m 2>&1";
    } else if (command == "df_root") {
        shellCommand = "df -h / 2>&1";
    } else if (command == "top_once") {
        shellCommand = "top -b -n 1 2>&1";
    } else if (command == "ps_gateway") {
        shellCommand = "ps | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver|EventEngine|SystemMonitor' 2>&1";
    } else if (command == "journal_tail") {
        shellCommand = "journalctl -n " + arg + " 2>&1";
    } else if (command == "systemctl_status") {
        if (!isSafeSystemdUnitName(arg)) {
            throw std::runtime_error("invalid service name");
        }
        shellCommand = "systemctl status " + arg + " 2>&1";
    } else if (command == "cellular_status") {
        shellCommand =
            "sh -c '"
            "echo \"# network interfaces\"; "
            "ip -br addr 2>&1 || true; "
            "echo; echo \"# route\"; "
            "ip route 2>&1 || true; "
            "echo; echo \"# dns\"; "
            "cat /etc/resolv.conf 2>&1 || true; "
            "echo; echo \"# modem devices\"; "
            "ls -l /dev/ttyUSB* /dev/cdc-wdm* 2>/dev/null || true; "
            "echo; echo \"# AT ports\"; "
            "for dev in /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB0 /dev/ttyUSB3 /dev/cdc-wdm*; do "
            "[ -e \"$dev\" ] && echo \"$dev present\"; "
            "done; "
            "echo \"AT detail is collected by SystemMonitor telemetry to avoid blocking shell diagnostics\"; "
            "echo; echo \"# mmcli\"; "
            "if command -v mmcli >/dev/null 2>&1; then mmcli -L 2>&1; mmcli -m 0 --output-keyvalue 2>&1; else echo mmcli_not_found; fi; "
            "echo; echo \"# traffic\"; "
            "cat /proc/net/dev 2>&1 || true"
            "'";
    } else {
        throw std::runtime_error("unsupported diag command");
    }

    return runShellCommand(shellCommand, exitCode);
}

}  // namespace edge_gateway
