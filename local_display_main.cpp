#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

std::atomic<bool> g_running(true);

void handleSignal(int) {
    g_running = false;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string basenameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string sanitizeProcessToken(std::string value) {
    for (auto& ch : value) {
        if (ch == '/' || ch == '\\' || ch == '.' || ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    return value;
}

void setProcessName(const std::string& name) {
#ifndef _WIN32
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#else
    (void)name;
#endif
}

std::string escapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    escaped += '?';
                } else {
                    escaped += ch;
                }
        }
    }
    return escaped;
}

std::string escapeScriptString(const std::string& value) {
    std::string escaped = escapeJson(value);
    std::string needle = "</script";
    std::string replacement = "<\\/script";
    std::size_t pos = 0;
    while ((pos = escaped.find(needle, pos)) != std::string::npos) {
        escaped.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return escaped;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> result;
    std::string current;
    std::istringstream input(text);
    while (std::getline(input, current, delimiter)) {
        if (!current.empty()) {
            result.push_back(current);
        }
    }
    return result;
}

std::string urlDecode(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const auto hex = text.substr(i + 1, 2);
            char* end = nullptr;
            const auto value = std::strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0') {
                result.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> result;
    for (const auto& item : split(query, '&')) {
        const auto pos = item.find('=');
        if (pos == std::string::npos) {
            result[urlDecode(item)] = "";
        } else {
            result[urlDecode(item.substr(0, pos))] = urlDecode(item.substr(pos + 1));
        }
    }
    return result;
}

std::vector<std::uint32_t> parseIndexes(const std::string& value) {
    std::vector<std::uint32_t> indexes;
    for (const auto& item : split(value, ',')) {
        try {
            indexes.push_back(static_cast<std::uint32_t>(std::stoul(item)));
        } catch (...) {
        }
    }
    return indexes;
}

template <typename T>
void appendUnique(std::vector<T>& values, std::unordered_set<T>& seen, const T& value) {
    if (seen.insert(value).second) {
        values.push_back(value);
    }
}

struct PointMeta {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string name;
    std::string category;
    std::string unit;
    std::string deviceName;
    std::string sharedMemoryName;
    bool fullUpload = false;
    bool reportOnChange = false;
    bool writable = false;
};

std::vector<edge_gateway::PointDefinition> effectiveMeterPoints(
    const edge_gateway::DeviceConfig& config,
    const edge_gateway::LogicalDeviceConfig& device
) {
    if (!device.points.empty()) {
        return device.points;
    }
    if (config.protocol.type != "dlt645_2007" || config.protocol.standardPointsFile.empty()) {
        return {};
    }
    return edge_gateway::ConfigLoader::loadDlt645StandardPointsFromFile(config.protocol.standardPointsFile);
}

std::unordered_map<std::uint32_t, PointMeta> buildPointMeta(
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs,
    const std::string& fallbackSharedMemoryName
) {
    std::unordered_map<std::uint32_t, PointMeta> result;
    for (const auto& config : deviceConfigs) {
        const auto sharedMemoryName = config.memoryStore.sharedMemoryName.empty()
            ? fallbackSharedMemoryName
            : config.memoryStore.sharedMemoryName;

        std::size_t meterIndex = 0;
        for (const auto& meter : config.meters) {
            auto points = effectiveMeterPoints(config, meter);
            if (config.protocol.type == "dlt645_2007" && meter.points.empty()) {
                const std::uint32_t indexBase = 200000U + static_cast<std::uint32_t>(meterIndex) * 10000U;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    points[i].index = indexBase + static_cast<std::uint32_t>(i);
                }
            }
            for (const auto& point : points) {
                PointMeta meta;
                meta.index = point.index;
                meta.machineCode = config.machineCode;
                meta.meterCode = meter.meterCode;
                meta.pointCode = point.pointCode;
                meta.name = point.name;
                meta.category = point.category;
                meta.unit = point.read.unit;
                meta.deviceName = meter.deviceName;
                meta.sharedMemoryName = sharedMemoryName;
                meta.fullUpload = point.fullUpload;
                meta.reportOnChange = point.reportOnChange;
                meta.writable = point.write.enable;
                result[meta.index] = meta;
            }
            ++meterIndex;
        }

        for (const auto& point : config.points) {
            PointMeta meta;
            meta.index = point.index;
            meta.machineCode = config.machineCode;
            meta.meterCode = config.meterCode;
            meta.pointCode = point.pointCode;
            meta.name = point.name;
            meta.category = point.category;
            meta.unit = point.read.unit;
            meta.deviceName = config.deviceName;
            meta.sharedMemoryName = sharedMemoryName;
            meta.fullUpload = point.fullUpload;
            meta.reportOnChange = point.reportOnChange;
            meta.writable = point.write.enable;
            result[meta.index] = meta;
        }
    }
    return result;
}

class LocalDisplayServer {
public:
    LocalDisplayServer(
        edge_gateway::LocalDisplayConfig config,
        std::string machineCode,
        edge_gateway::PointStoreRouter& router,
        std::unordered_map<std::uint32_t, PointMeta> pointMeta
    )
        : config_(std::move(config)),
          machineCode_(std::move(machineCode)),
          router_(router),
          pointMeta_(std::move(pointMeta)) {
        if (config_.refreshIntervalMs < 100) {
            config_.refreshIntervalMs = 100;
        }
        if (config_.maxPointsPerFrame == 0) {
            config_.maxPointsPerFrame = 500;
        }
    }

    void run() {
#ifdef _WIN32
        throw std::runtime_error("LocalDisplay is only supported on Linux");
#else
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            throw std::runtime_error("failed to create listen socket");
        }

        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
        if (::inet_pton(AF_INET, config_.bindHost.c_str(), &address.sin_addr) != 1) {
            throw std::runtime_error("invalid localDisplay.bindHost: " + config_.bindHost);
        }

        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            throw std::runtime_error("failed to bind LocalDisplay on " + config_.bindHost + ":" + std::to_string(config_.port));
        }
        if (::listen(listenFd_, 32) < 0) {
            throw std::runtime_error("failed to listen LocalDisplay socket");
        }

        std::cout << "local display listening"
                  << " http://" << config_.bindHost << ":" << config_.port
                  << " machine=" << machineCode_
                  << " routes=" << router_.routes().size()
                  << std::endl;

        while (g_running) {
            sockaddr_in clientAddress;
            socklen_t clientLength = sizeof(clientAddress);
            const int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
            if (clientFd < 0) {
                if (g_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            std::thread(&LocalDisplayServer::handleClient, this, clientFd).detach();
        }
        ::close(listenFd_);
        listenFd_ = -1;
#endif
    }

private:
#ifndef _WIN32
    void handleClient(int clientFd) {
        char buffer[4096];
        const ssize_t received = ::recv(clientFd, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            ::close(clientFd);
            return;
        }
        buffer[received] = '\0';
        const std::string request(buffer);
        const auto firstLineEnd = request.find("\r\n");
        const auto firstLine = firstLineEnd == std::string::npos ? request : request.substr(0, firstLineEnd);
        std::istringstream line(firstLine);
        std::string method;
        std::string uri;
        std::string version;
        line >> method >> uri >> version;

        if (method != "GET") {
            sendResponse(clientFd, 405, "text/plain; charset=utf-8", "method not allowed\n");
            ::close(clientFd);
            return;
        }

        std::string path = uri;
        std::string query;
        const auto queryPos = uri.find('?');
        if (queryPos != std::string::npos) {
            path = uri.substr(0, queryPos);
            query = uri.substr(queryPos + 1);
        }
        const auto params = parseQuery(query);

        if (path == "/" || path == "/index.html") {
            sendResponse(clientFd, 200, "text/html; charset=utf-8", indexHtml());
        } else if (path == "/api/health") {
            sendResponse(clientFd, 200, "application/json; charset=utf-8", healthJson());
        } else if (path == "/api/pages") {
            sendResponse(clientFd, 200, "application/json; charset=utf-8", pagesJson());
        } else if (path == "/api/points") {
            sendResponse(clientFd, 200, "application/json; charset=utf-8", pointsJson());
        } else if (path == "/api/frame") {
            const auto pageCode = findParam(params, "pageCode");
            const auto indexesText = findParam(params, "indexes");
            const auto indexes = indexesText.empty() ? selectIndexes(pageCode) : parseIndexes(indexesText);
            sendResponse(clientFd, 200, "application/json; charset=utf-8", frameJson(pageCode, indexes));
        } else if (path == "/api/stream") {
            handleStream(clientFd, findParam(params, "pageCode"), parseIndexes(findParam(params, "indexes")));
            return;
        } else {
            sendResponse(clientFd, 404, "text/plain; charset=utf-8", "not found\n");
        }
        ::close(clientFd);
    }

    std::string findParam(const std::map<std::string, std::string>& params, const std::string& key) const {
        const auto it = params.find(key);
        return it == params.end() ? std::string() : it->second;
    }

    bool sendAll(int fd, const std::string& data) const {
        const char* cursor = data.data();
        std::size_t left = data.size();
        while (left > 0) {
            const ssize_t sent = ::send(fd, cursor, left, 0);
            if (sent <= 0) {
                return false;
            }
            cursor += sent;
            left -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    void sendResponse(int fd, int status, const std::string& contentType, const std::string& body) const {
        std::ostringstream header;
        header << "HTTP/1.1 " << status << " " << statusText(status) << "\r\n"
               << "Content-Type: " << contentType << "\r\n"
               << "Cache-Control: no-store\r\n"
               << "Access-Control-Allow-Origin: *\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n";
        sendAll(fd, header.str());
        sendAll(fd, body);
    }

    void handleStream(int fd, const std::string& pageCode, std::vector<std::uint32_t> indexes) {
        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n\r\n";
        if (!sendAll(fd, header)) {
            ::close(fd);
            return;
        }
        if (indexes.empty()) {
            indexes = selectIndexes(pageCode);
        }
        while (g_running) {
            const auto payload = frameJson(pageCode, indexes);
            if (!sendAll(fd, "event: frame\n")) {
                break;
            }
            if (!sendAll(fd, "data: " + payload + "\n\n")) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.refreshIntervalMs));
        }
        ::close(fd);
    }

    std::string statusText(int status) const {
        switch (status) {
            case 200: return "OK";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            default: return "Error";
        }
    }
#endif

    std::vector<std::uint32_t> selectIndexes(const std::string& pageCode) const {
        std::vector<std::uint32_t> indexes;
        std::unordered_set<std::uint32_t> seen;

        if (config_.showOnlyConfiguredPoints && !config_.screens.empty()) {
            for (const auto& screen : config_.screens) {
                if (!pageCode.empty() && screen.screenCode != pageCode) {
                    continue;
                }
                for (const auto& widget : screen.widgets) {
                    appendWidgetIndexes(indexes, seen, widget);
                }
            }
        }

        if (config_.showOnlyConfiguredPoints && indexes.empty() && !config_.pages.empty()) {
            for (const auto& page : config_.pages) {
                if (!pageCode.empty() && page.pageCode != pageCode) {
                    continue;
                }
                for (const auto& group : page.groups) {
                    for (const auto index : group.pointIndexes) {
                        appendUnique(indexes, seen, index);
                    }
                }
            }
        }

        if (indexes.empty()) {
            for (const auto index : router_.allIndexes()) {
                appendUnique(indexes, seen, index);
            }
        }

        if (indexes.size() > config_.maxPointsPerFrame) {
            indexes.resize(config_.maxPointsPerFrame);
        }
        return indexes;
    }

    void appendWidgetIndexes(
        std::vector<std::uint32_t>& indexes,
        std::unordered_set<std::uint32_t>& seen,
        const edge_gateway::LocalDisplayWidgetConfig& widget
    ) const {
        if (widget.type == "pointTable" || widget.type == "alarmSummary") {
            for (const auto index : widget.pointIndexes) {
                appendUnique(indexes, seen, index);
            }
            return;
        }
        if (widget.pointIndex != 0) {
            appendUnique(indexes, seen, widget.pointIndex);
        }
    }

    std::string healthJson() const {
        std::ostringstream out;
        const auto stats = router_.getStoreStats();
        out << "{\"success\":true"
            << ",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"configuredPointCount\":" << pointMeta_.size()
            << ",\"routeCount\":" << router_.routes().size()
            << ",\"openedSharedMemoryCount\":" << stats.size()
            << ",\"refreshIntervalMs\":" << config_.refreshIntervalMs
            << ",\"maxPointsPerFrame\":" << static_cast<unsigned long long>(config_.maxPointsPerFrame)
            << ",\"stores\":[";
        for (std::size_t i = 0; i < stats.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << "{\"sharedMemoryName\":\"" << escapeJson(stats[i].sharedMemoryName) << "\""
                << ",\"latestCount\":" << stats[i].latestCount
                << ",\"latestCapacity\":" << stats[i].latestCapacity
                << ",\"pendingWriteCount\":" << stats[i].pendingWriteCount
                << "}";
        }
        out << "],\"ts\":" << nowMs() << "}";
        return out.str();
    }

    std::string pagesJson() const {
        std::ostringstream out;
        out << "{\"pages\":[";
        if (!config_.screens.empty()) {
            for (std::size_t p = 0; p < config_.screens.size(); ++p) {
                if (p > 0) {
                    out << ",";
                }
                const auto& screen = config_.screens[p];
                out << "{\"pageCode\":\"" << escapeJson(screen.screenCode) << "\""
                    << ",\"title\":\"" << escapeJson(screen.title) << "\""
                    << ",\"groups\":[{\"title\":\"组件点位\",\"pointIndexes\":[";
                std::vector<std::uint32_t> indexes;
                std::unordered_set<std::uint32_t> seen;
                for (const auto& widget : screen.widgets) {
                    appendWidgetIndexes(indexes, seen, widget);
                }
                for (std::size_t i = 0; i < indexes.size(); ++i) {
                    if (i > 0) {
                        out << ",";
                    }
                    out << indexes[i];
                }
                out << "]}]}";
            }
            out << "]}";
            return out.str();
        }
        for (std::size_t p = 0; p < config_.pages.size(); ++p) {
            if (p > 0) {
                out << ",";
            }
            const auto& page = config_.pages[p];
            out << "{\"pageCode\":\"" << escapeJson(page.pageCode) << "\""
                << ",\"title\":\"" << escapeJson(page.title) << "\""
                << ",\"groups\":[";
            for (std::size_t g = 0; g < page.groups.size(); ++g) {
                if (g > 0) {
                    out << ",";
                }
                const auto& group = page.groups[g];
                out << "{\"title\":\"" << escapeJson(group.title) << "\",\"pointIndexes\":[";
                for (std::size_t i = 0; i < group.pointIndexes.size(); ++i) {
                    if (i > 0) {
                        out << ",";
                    }
                    out << group.pointIndexes[i];
                }
                out << "]}";
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

    void appendStringArrayJson(std::ostringstream& out, const std::vector<std::string>& values) const {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << "\"" << escapeJson(values[i]) << "\"";
        }
        out << "]";
    }

    void appendIndexArrayJson(std::ostringstream& out, const std::vector<std::uint32_t>& values) const {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << values[i];
        }
        out << "]";
    }

    void appendWidgetConfigJson(std::ostringstream& out, const edge_gateway::LocalDisplayWidgetConfig& widget) const {
        out << "{\"id\":\"" << escapeJson(widget.id) << "\""
            << ",\"type\":\"" << escapeJson(widget.type) << "\""
            << ",\"title\":\"" << escapeJson(widget.title) << "\""
            << ",\"text\":\"" << escapeJson(widget.text) << "\""
            << ",\"valueFormat\":\"" << escapeJson(widget.valueFormat) << "\""
            << ",\"bind\":{";
        bool hasBindField = false;
        if (widget.pointIndex != 0) {
            out << "\"index\":" << widget.pointIndex;
            hasBindField = true;
        }
        if (!widget.pointIndexes.empty()) {
            if (hasBindField) {
                out << ",";
            }
            out << "\"indexes\":";
            appendIndexArrayJson(out, widget.pointIndexes);
        }
        out << "},\"columns\":";
        appendStringArrayJson(out, widget.columns);
        out << ",\"position\":{"
            << "\"x\":" << widget.grid.col
            << ",\"y\":" << widget.grid.row
            << ",\"w\":" << widget.grid.colSpan
            << ",\"h\":" << widget.grid.rowSpan
            << "}}";
    }

    std::string screensJsonArray() const {
        std::ostringstream out;
        out << "[";
        for (std::size_t i = 0; i < config_.screens.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            const auto& screen = config_.screens[i];
            out << "{\"screenCode\":\"" << escapeJson(screen.screenCode) << "\""
                << ",\"title\":\"" << escapeJson(screen.title) << "\""
                << ",\"layout\":{"
                << "\"type\":\"" << escapeJson(screen.layout.type) << "\""
                << ",\"columns\":" << screen.layout.columns
                << ",\"rowHeight\":" << screen.layout.rowHeight
                << ",\"gap\":" << screen.layout.gap
                << "},\"widgets\":[";
            for (std::size_t w = 0; w < screen.widgets.size(); ++w) {
                if (w > 0) {
                    out << ",";
                }
                appendWidgetConfigJson(out, screen.widgets[w]);
            }
            out << "]}";
        }
        out << "]";
        return out.str();
    }

    std::string pointsJson() const {
        std::ostringstream out;
        const auto indexes = router_.allIndexes();
        out << "{\"machineCode\":\"" << escapeJson(machineCode_) << "\",\"points\":[";
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            appendPointMetaJson(out, indexes[i]);
        }
        out << "]}";
        return out.str();
    }

    std::string frameJson(const std::string& pageCode, const std::vector<std::uint32_t>& indexes) {
        const auto ts = nowMs();
        const auto seq = ++sequence_;
        std::ostringstream out;
        out << "{\"type\":\"local-display-frame\""
            << ",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"pageCode\":\"" << escapeJson(pageCode.empty() ? "overview" : pageCode) << "\""
            << ",\"seq\":" << seq
            << ",\"ts\":" << ts
            << ",\"points\":[";
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            appendPointValueJson(out, indexes[i], ts);
        }
        out << "]}";
        return out.str();
    }

    void appendPointMetaJson(std::ostringstream& out, std::uint32_t index) const {
        const auto route = router_.routeByIndex(index);
        const auto metaIt = pointMeta_.find(index);
        const PointMeta* meta = metaIt == pointMeta_.end() ? nullptr : &metaIt->second;
        out << "{\"index\":" << index
            << ",\"machineCode\":\"" << escapeJson(meta != nullptr ? meta->machineCode : (route ? route->machineCode : "")) << "\""
            << ",\"meterCode\":\"" << escapeJson(meta != nullptr ? meta->meterCode : (route ? route->meterCode : "")) << "\""
            << ",\"pointCode\":\"" << escapeJson(meta != nullptr ? meta->pointCode : (route ? route->pointCode : "")) << "\""
            << ",\"name\":\"" << escapeJson(meta != nullptr ? meta->name : "") << "\""
            << ",\"category\":\"" << escapeJson(meta != nullptr ? meta->category : "") << "\""
            << ",\"unit\":\"" << escapeJson(meta != nullptr ? meta->unit : "") << "\""
            << ",\"deviceName\":\"" << escapeJson(meta != nullptr ? meta->deviceName : "") << "\""
            << ",\"sharedMemoryName\":\"" << escapeJson(meta != nullptr ? meta->sharedMemoryName : (route ? route->sharedMemoryName : "")) << "\""
            << ",\"writable\":" << ((meta != nullptr ? meta->writable : (route ? route->writable : false)) ? "true" : "false")
            << "}";
    }

    void appendPointValueJson(std::ostringstream& out, std::uint32_t index, std::int64_t ts) const {
        const auto value = router_.getLatestByIndex(index, ts);
        out << "{";
        appendPointMetaFields(out, index);
        if (value) {
            out << ",\"hasValue\":true"
                << ",\"value\":" << value->value
                << ",\"quality\":" << value->quality
                << ",\"valueTs\":" << value->ts
                << ",\"expireAt\":" << value->expireAt
                << ",\"stale\":" << (value->stale ? "true" : "false");
        } else {
            out << ",\"hasValue\":false"
                << ",\"value\":null"
                << ",\"quality\":0"
                << ",\"valueTs\":0"
                << ",\"expireAt\":0"
                << ",\"stale\":true";
        }
        out << "}";
    }

    void appendPointMetaFields(std::ostringstream& out, std::uint32_t index) const {
        const auto route = router_.routeByIndex(index);
        const auto metaIt = pointMeta_.find(index);
        const PointMeta* meta = metaIt == pointMeta_.end() ? nullptr : &metaIt->second;
        out << "\"index\":" << index
            << ",\"machineCode\":\"" << escapeJson(meta != nullptr ? meta->machineCode : (route ? route->machineCode : "")) << "\""
            << ",\"meterCode\":\"" << escapeJson(meta != nullptr ? meta->meterCode : (route ? route->meterCode : "")) << "\""
            << ",\"pointCode\":\"" << escapeJson(meta != nullptr ? meta->pointCode : (route ? route->pointCode : "")) << "\""
            << ",\"name\":\"" << escapeJson(meta != nullptr ? meta->name : "") << "\""
            << ",\"category\":\"" << escapeJson(meta != nullptr ? meta->category : "") << "\""
            << ",\"unit\":\"" << escapeJson(meta != nullptr ? meta->unit : "") << "\""
            << ",\"deviceName\":\"" << escapeJson(meta != nullptr ? meta->deviceName : "") << "\"";
    }

    std::string defaultTemplateHtml() const {
        return R"TEMPLATE(<div class="local-page">
  <header>
    <div><h1>Gateway Local Display</h1><div class="sub">machine: {{machineCode}}</div></div>
    <div class="sub">{{clock}}</div>
  </header>
  <main class="wrap">
    <div class="cards">
      <div class="card"><div class="label">Points</div><div class="num">{{pointCount}}</div></div>
      <div class="card"><div class="label">Online</div><div class="num ok">{{onlineCount}}</div></div>
      <div class="card"><div class="label">Stale/Bad</div><div class="num bad">{{badCount}}</div></div>
      <div class="card"><div class="label">Last Update</div><div class="num">{{seq}}</div></div>
    </div>
    <div class="toolbar">
      <span class="select-like">{{pageCode}}</span>
      <span class="input-like">{{filterText}}</span>
    </div>
    <div class="table-wrap">
      <table>
        <thead><tr><th>Index</th><th>Meter</th><th>Point</th><th>Name</th><th>Value</th><th>Unit</th><th>Quality</th><th>Time</th></tr></thead>
        <tbody>{{pointsRows}}</tbody>
      </table>
    </div>
  </main>
</div>)TEMPLATE";
    }

    std::string defaultTemplateCss() const {
        return R"TEMPLATE(:root{--bg:#101418;--panel:#182129;--line:#26323d;--text:#e8f0f2;--muted:#8fa4ad;--ok:#16c784;--bad:#ff5c5c;--warn:#f5a623;--accent:#56b6ff}
*{box-sizing:border-box}body{margin:0;background:#101820;color:var(--text);font-family:"Segoe UI","Noto Sans SC",sans-serif}
header{padding:18px 24px;border-bottom:1px solid var(--line);display:flex;gap:16px;align-items:center;justify-content:space-between}
h1{margin:0;font-size:24px;letter-spacing:.04em}.sub{color:var(--muted);font-size:13px}.wrap{padding:18px 24px}
.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-bottom:16px}.card{background:#182129;border:1px solid var(--line);border-radius:14px;padding:14px}
.label{color:var(--muted);font-size:12px}.num{font-size:28px;margin-top:6px}.ok{color:var(--ok)}.bad{color:var(--bad)}
.toolbar{display:flex;gap:8px;align-items:center;margin:12px 0}.select-like,.input-like{background:#0f171e;color:var(--text);border:1px solid var(--line);border-radius:8px;padding:8px 10px;min-width:160px}
.table-wrap{overflow:auto;max-height:calc(100vh - 260px)}table{width:100%;border-collapse:collapse;background:#182129;border:1px solid var(--line);border-radius:14px;overflow:hidden}
th,td{padding:10px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap}th{color:var(--muted);font-weight:500;background:#151d25}
tr.stale td{color:var(--warn)}tr.bad td{color:var(--bad)}@media(max-width:900px){.cards{grid-template-columns:repeat(2,minmax(0,1fr))}.wrap{padding:12px}header{padding:14px}})TEMPLATE";
    }

    std::string defaultComponentCss() const {
        return R"TEMPLATE(:root{--bg:#101820;--panel:#182129;--line:#26323d;--text:#e8f0f2;--muted:#8fa4ad;--ok:#16c784;--bad:#ff5c5c;--warn:#f5a623;--accent:#56b6ff}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(135deg,#0b1117,#17222c);color:var(--text);font-family:"Segoe UI","Noto Sans SC",sans-serif}
.ld-screen{min-height:100vh;padding:18px 22px}.ld-header{display:flex;align-items:flex-end;justify-content:space-between;margin-bottom:14px}.ld-header h1{margin:0;font-size:26px}.ld-sub{color:var(--muted);font-size:13px}
.ld-grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));grid-auto-rows:64px;gap:12px}.ld-widget{min-width:0;overflow:hidden;border:1px solid var(--line);border-radius:16px;background:rgba(24,33,41,.92);box-shadow:0 12px 28px rgba(0,0,0,.18)}
.ld-title{display:flex;align-items:center;justify-content:space-between;padding:0 18px;font-size:24px;font-weight:700;background:rgba(15,23,30,.92)}.ld-title span{font-size:13px;color:var(--muted);font-weight:400}
.ld-card{padding:14px}.ld-widget-title{color:var(--muted);font-size:13px;margin-bottom:8px}.ld-value{font-size:34px;line-height:1.1}.ld-meta,.ld-status{margin-top:8px;color:var(--muted);font-size:12px}.ld-card.ok .ld-value{color:var(--ok)}.ld-card.bad .ld-value{color:var(--bad)}.ld-card.stale .ld-value{color:var(--warn)}
.ld-table-widget{padding:12px}.ld-table-wrap{height:calc(100% - 28px);overflow:auto}table{width:100%;border-collapse:collapse}th,td{padding:8px 10px;border-bottom:1px solid var(--line);white-space:nowrap;text-align:left}th{color:var(--muted);font-weight:500;background:#151d25}tr.bad td{color:var(--bad)}tr.stale td{color:var(--warn)})TEMPLATE";
    }

    std::string configuredTemplateHtml() const {
        if (!config_.viewTemplate.enabled || config_.viewTemplate.html.empty()) {
            return defaultTemplateHtml();
        }
        return config_.viewTemplate.html;
    }

    std::string configuredTemplateCss() const {
        if (!config_.viewTemplate.enabled || config_.viewTemplate.css.empty()) {
            return defaultTemplateCss();
        }
        return config_.viewTemplate.css;
    }

    std::string indexHtml() const {
        const int templateRefreshMs = std::max(5000, config_.viewTemplate.refreshIntervalMs);
        std::ostringstream out;
        out << R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Gateway Local Display</title>
  <style id="templateStyle"></style>
</head>
<body>
  <div id="localDisplayRoot"></div>
  <script>
    const templateHtml = ")HTML" << escapeScriptString(configuredTemplateHtml()) << R"HTML(";
    const templateCss = ")HTML" << escapeScriptString(configuredTemplateCss()) << R"HTML(";
    const componentCss = ")HTML" << escapeScriptString(defaultComponentCss()) << R"HTML(";
    const displayScreens = )HTML" << screensJsonArray() << R"HTML(;
    const templateRefreshIntervalMs = )HTML" << templateRefreshMs << R"HTML(;
    const state={frame:{machineCode:"",pageCode:"overview",seq:"-",points:[]},page:new URLSearchParams(location.search).get("pageCode")||"overview",filter:"",stream:null,polling:false};
    const root=document.getElementById("localDisplayRoot");
    const styleEl=document.getElementById("templateStyle");
    let lastRenderedCss="",lastRenderSignature="";
    function esc(v){return String(v??"").replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;").replace(/'/g,"&#39;")}
    function fmtTs(ts){return ts?new Date(ts).toLocaleTimeString():"-"}
    function fmtVal(v){return v===null||v===undefined?"-":Number(v).toFixed(3).replace(/\.?0+$/,"")}
    function status(p){if(!p||!p.hasValue)return{className:"bad",text:"bad",color:"#ff5c5c"};if(p.stale)return{className:"stale",text:"stale",color:"#f5a623"};if(p.quality!==1)return{className:"bad",text:"bad",color:"#ff5c5c"};return{className:"ok",text:"ok",color:"#16c784"}}
    function filteredPoints(){const f=state.filter.toLowerCase();const pts=state.frame.points||[];return pts.filter(p=>!f||String(p.index).includes(f)||(p.meterCode||"").toLowerCase().includes(f)||(p.pointCode||"").toLowerCase().includes(f)||(p.name||"").toLowerCase().includes(f))}
    function pointField(p,field){const s=status(p);switch(field||"value"){case"index":return p?.index??"";case"meterCode":return p?.meterCode||"";case"pointCode":return p?.pointCode||"";case"name":return p?.name||"";case"unit":return p?.unit||"";case"value":return p?.hasValue?fmtVal(p.value):"-";case"rawValue":return p?.hasValue?p.value:"";case"quality":return p?.quality??"";case"time":return fmtTs(p?.valueTs);case"timestamp":return p?.valueTs||"";case"stale":return p?.stale?"true":"false";case"hasValue":return p?.hasValue?"true":"false";case"statusClass":return s.className;case"statusText":return s.text;case"statusColor":return s.color;default:return p&&Object.prototype.hasOwnProperty.call(p,field)?p[field]:""}}
    function rowsHtml(){return filteredPoints().map(p=>{const s=status(p);return `<tr class="${esc(s.className)}"><td>${esc(p.index)}</td><td>${esc(p.meterCode||"")}</td><td>${esc(p.pointCode||"")}</td><td>${esc(p.name||"")}</td><td>${esc(p.hasValue?fmtVal(p.value):"-")}</td><td>${esc(p.unit||"")}</td><td>${esc(p.quality)}</td><td>${esc(fmtTs(p.valueTs))}</td></tr>`}).join("")}
    function resolve(key){key=String(key||"").trim();const frame=state.frame;const pts=frame.points||[];if(!key)return"";if(key==="pointsRows")return{rawHtml:rowsHtml()};if(key==="machineCode")return frame.machineCode||"-";if(key==="pageCode")return frame.pageCode||state.page||"overview";if(key==="clock"||key==="now")return new Date().toLocaleString();if(key==="seq"||key==="lastUpdate")return frame.seq||"-";if(key==="pointCount")return pts.length;if(key==="onlineCount")return pts.filter(p=>p.hasValue&&p.quality===1&&!p.stale).length;if(key==="badCount")return pts.filter(p=>!p.hasValue||p.quality!==1||p.stale).length;if(key==="filterText")return state.filter||"filter meter / point / name";const m=key.match(/^(?:p|point)\.(\d+)\.?([A-Za-z0-9_]+)?$/);if(m){const p=pts.find(x=>Number(x.index)===Number(m[1]));return pointField(p,m[2]||"value")}return""}
    function renderText(text,escapeValues){return String(text||"").replace(/\{\{\s*([^}]+?)\s*\}\}/g,(_,path)=>{const value=resolve(path);if(value&&typeof value==="object"&&Object.prototype.hasOwnProperty.call(value,"rawHtml"))return value.rawHtml;return escapeValues?esc(value):String(value??"")})}
    function activeScreen(){if(!Array.isArray(displayScreens)||!displayScreens.length)return null;return displayScreens.find(s=>s.screenCode===state.page)||displayScreens[0]}
    function columnsOf(w){return Array.isArray(w?.columns)&&w.columns.length?w.columns:["index","meterCode","pointCode","name","value","unit","quality","time"]}
    function labelOf(c){return {index:"Index",meterCode:"仪表",pointCode:"点位",name:"名称",value:"值",unit:"单位",quality:"质量",time:"时间",statusText:"状态"}[c]||c}
    function pointByIndex(index){return (state.frame.points||[]).find(p=>Number(p.index)===Number(index))||{index:Number(index||0),hasValue:false,quality:0,stale:true,value:null,valueTs:0}}
    function gridStyle(w){const p=w?.position||{};const x=Math.max(0,Number(p.x||0));const y=Math.max(0,Number(p.y||0));const ww=Math.max(1,Number(p.w||((w?.type==="pointTable")?12:3)));const h=Math.max(1,Number(p.h||((w?.type==="pointTable")?5:2)));return `grid-column:${x+1} / span ${ww};grid-row:${y+1} / span ${h};`}
    function widgetValue(w,p){if(!p||!p.hasValue)return"-";if(w?.valueFormat==="boolOnline")return Number(p.value)===1?"在线":"离线";if(w?.valueFormat==="raw")return String(p.value);return fmtVal(p.value)}
    function renderTable(w){const cols=columnsOf(w);const indexes=(w?.bind?.indexes)||[];const head=cols.map(c=>`<th>${esc(labelOf(c))}</th>`).join("");const rows=indexes.map(i=>{const p=pointByIndex(i);const s=status(p);return `<tr class="${esc(s.className)}">${cols.map(c=>`<td>${esc(pointField(p,c))}</td>`).join("")}</tr>`}).join("");return `<div class="ld-table-wrap"><table><thead><tr>${head}</tr></thead><tbody>${rows}</tbody></table></div>`}
    function renderWidget(w){const st=gridStyle(w);if(w?.type==="groupTitle")return `<section class="ld-widget ld-title" style="${st}"><div>${esc(w.text||w.title||"")}</div><span data-ld-clock>${esc(new Date().toLocaleString())}</span></section>`;if(w?.type==="pointTable"||w?.type==="alarmSummary")return `<section class="ld-widget ld-table-widget" style="${st}"><div class="ld-widget-title">${esc(w.title||"")}</div>${renderTable(w)}</section>`;const p=pointByIndex(w?.bind?.index);const s=status(p);return `<section class="ld-widget ld-card ${esc(s.className)}" style="${st}"><div class="ld-widget-title">${esc(w.title||"")}</div><div class="ld-value">${esc(widgetValue(w,p))}</div><div class="ld-meta">${esc(p.name||p.pointCode||p.index||"")} ${esc(p.unit||"")}</div><div class="ld-status">${esc(s.text)} / Q=${esc(p.quality??"")} / ${esc(fmtTs(p.valueTs))}</div></section>`}
    function renderComponents(){const screen=activeScreen();if(!screen)return"";const layout=screen.layout||{};const cols=Math.max(1,Number(layout.columns||12));const rowHeight=Math.max(24,Number(layout.rowHeight||64));const gap=Math.max(0,Number(layout.gap||12));const css=componentCss+`.ld-grid{grid-template-columns:repeat(${cols},minmax(0,1fr));grid-auto-rows:${rowHeight}px;gap:${gap}px}`;if(css!==lastRenderedCss){styleEl.textContent=css;lastRenderedCss=css}return `<main class="ld-screen"><header class="ld-header"><div><h1>${esc(screen.title||"Gateway Local Display")}</h1><div class="ld-sub">machine: ${esc(state.frame.machineCode||"-")}</div></div><div class="ld-sub" data-ld-clock>${esc(new Date().toLocaleString())}</div></header><section class="ld-grid">${(screen.widgets||[]).map(renderWidget).join("")}</section></main>`}
    function renderSignature(){const pts=state.frame.points||[];return [state.page,state.filter,state.frame.machineCode||"",pts.length,pts.map(p=>[p.index,p.hasValue?1:0,p.value,p.quality,p.stale?1:0,p.meterCode||"",p.pointCode||"",p.name||"",p.unit||"",Math.floor(Number(p.valueTs||0)/60000)].join(":")).join("|")].join("#")}
    function refreshClockText(){const text=new Date().toLocaleString();document.querySelectorAll("[data-ld-clock]").forEach(el=>{el.textContent=text})}
    function render(force){const signature=renderSignature();if(!force&&signature===lastRenderSignature){refreshClockText();return}lastRenderSignature=signature;if(Array.isArray(displayScreens)&&displayScreens.length){root.innerHTML=renderComponents();return}const css=renderText(templateCss,false);if(css!==lastRenderedCss){styleEl.textContent=css;lastRenderedCss=css}root.innerHTML=renderText(templateHtml,true)}
    function applyFrame(frame){state.frame=frame||state.frame;render(false)}
    function connectStream(){if(state.stream){state.stream.close();state.stream=null}state.polling=false;if(!window.EventSource){poll();return}const es=new EventSource("/api/stream?pageCode="+encodeURIComponent(state.page));state.stream=es;es.addEventListener("frame",ev=>applyFrame(JSON.parse(ev.data)));es.onerror=()=>{es.close();state.stream=null;setTimeout(poll,1000)}}
    async function poll(){if(state.stream||state.polling)return;state.polling=true;try{const res=await fetch("/api/frame?pageCode="+encodeURIComponent(state.page),{cache:"no-store"});applyFrame(await res.json())}catch(e){}state.polling=false;if(!state.stream)setTimeout(poll,1000)}
    setInterval(()=>{if(Array.isArray(displayScreens)&&displayScreens.length){refreshClockText()}else{render(true)}},templateRefreshIntervalMs);
    connectStream();
  </script>
</body>
</html>
)HTML";
        return out.str();
    }

    edge_gateway::LocalDisplayConfig config_;
    std::string machineCode_;
    edge_gateway::PointStoreRouter& router_;
    std::unordered_map<std::uint32_t, PointMeta> pointMeta_;
    std::atomic<std::uint64_t> sequence_{0};
    int listenFd_ = -1;
};

std::vector<std::string> collectSharedMemoryNames(
    const edge_gateway::AppConfig& appConfig,
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    auto addName = [&](const std::string& name) {
        if (!name.empty() && seen.insert(name).second) {
            names.push_back(name);
        }
    };
    for (const auto& name : appConfig.localDisplay.sharedMemoryNames) {
        addName(name);
    }
    if (names.empty()) {
        for (const auto& name : appConfig.mqttDriver.sharedMemoryNames) {
            addName(name);
        }
        addName(appConfig.mqttDriver.sharedMemoryName);
    }
    for (const auto& config : deviceConfigs) {
        addName(config.memoryStore.sharedMemoryName);
    }
    addName(appConfig.cameraService.sharedMemoryName);
    if (names.empty()) {
        names.push_back("gateway_point_store");
    }
    return names;
}

std::string resolveMachineCode(
    const edge_gateway::DeviceIdentity& identity,
    const edge_gateway::AppConfig& appConfig,
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    if (!identity.machineCode.empty()) {
        return identity.machineCode;
    }
    if (!appConfig.mqtt.clientId.empty()) {
        return appConfig.mqtt.clientId;
    }
    for (const auto& config : deviceConfigs) {
        if (!config.machineCode.empty()) {
            return config.machineCode;
        }
    }
    return "UNKNOWN";
}

void printUsage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --app-config <app.json>"
              << " [--bind-host 127.0.0.1]"
              << " [--port 18080]"
              << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    std::string appConfigPath = "config/runtime/apps/monitor-service.json";
    std::string bindHost;
    int port = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if (arg == "--app-config") {
            appConfigPath = requireValue(arg);
        } else if (arg == "--bind-host") {
            bindHost = requireValue(arg);
        } else if (arg == "--port") {
            port = std::stoi(requireValue(arg));
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    setProcessName("local_display");

    try {
        auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(appConfigPath);
        edge_gateway::DeviceIdentity identity;
        if (!appConfig.identityConfigFile.empty()) {
            try {
                identity = edge_gateway::ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
            } catch (const std::exception& ex) {
                std::cerr << "load identity failed: " << ex.what() << std::endl;
            }
        }
        auto deviceConfigs = appConfig.identityConfigFile.empty()
            ? edge_gateway::ConfigLoader::loadMany(appConfig.deviceConfigFiles)
            : edge_gateway::ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);

        if (!bindHost.empty()) {
            appConfig.localDisplay.bindHost = bindHost;
        }
        if (port > 0) {
            appConfig.localDisplay.port = port;
        }

        if (!appConfig.localDisplay.enabled) {
            std::cout << "local display disabled appConfig=" << appConfigPath << std::endl;
            return 0;
        }

        const auto sharedMemoryNames = collectSharedMemoryNames(appConfig, deviceConfigs);
        std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
        edge_gateway::PointStoreRouter router;
        for (const auto& name : sharedMemoryNames) {
            stores.emplace_back(new edge_gateway::MemoryPointStore(name));
            router.addStore(name, *stores.back());
        }
        const auto fallbackSharedMemoryName = sharedMemoryNames.empty()
            ? std::string("gateway_point_store")
            : sharedMemoryNames.front();
        const auto machineCode = resolveMachineCode(identity, appConfig, deviceConfigs);
        router.addRoutesFromDeviceConfigs(deviceConfigs, fallbackSharedMemoryName);
        router.addRoutesFromCameraServiceConfig(appConfig.cameraService, machineCode);
        const auto pointMeta = buildPointMeta(deviceConfigs, fallbackSharedMemoryName);

        LocalDisplayServer server(appConfig.localDisplay, machineCode, router, pointMeta);
        server.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local display failed: " << ex.what() << std::endl;
        return 1;
    }
}
