#include <chrono>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
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

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u00";
                    const char* hex = "0123456789abcdef";
                    const auto valueByte = static_cast<unsigned char>(ch);
                    out << hex[(valueByte >> 4) & 0x0f] << hex[valueByte & 0x0f];
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            result += "'\\''";
        } else {
            result += ch;
        }
    }
    result += "'";
    return result;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    if (base.empty()) {
        return path;
    }
    if (path.empty()) {
        return base;
    }
    if (base.back() == '/' && path.front() == '/') {
        return base + path.substr(1);
    }
    if (base.back() != '/' && path.front() != '/') {
        return base + "/" + path;
    }
    return base + path;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isUnreservedUrlChar(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

std::string urlEncode(const std::string& value) {
    std::ostringstream out;
    const char* hex = "0123456789ABCDEF";
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (isUnreservedUrlChar(byte)) {
            out << ch;
        } else {
            out << '%' << hex[(byte >> 4) & 0x0f] << hex[byte & 0x0f];
        }
    }
    return out.str();
}

std::string insertUrlUserInfo(const std::string& url, const std::string& username, const std::string& password) {
    if (url.empty() || username.empty()) {
        return url;
    }
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return url;
    }
    const auto authorityStart = schemeEnd + 3;
    const auto authorityEnd = url.find('/', authorityStart);
    const auto hostEnd = authorityEnd == std::string::npos ? url.size() : authorityEnd;
    const auto at = url.find('@', authorityStart);
    if (at != std::string::npos && at < hostEnd) {
        return url;
    }
    std::string userInfo = urlEncode(username);
    if (!password.empty()) {
        userInfo += ":" + urlEncode(password);
    }
    userInfo += "@";
    return url.substr(0, authorityStart) + userInfo + url.substr(authorityStart);
}

std::string appendQueryParam(const std::string& url, const std::string& key, const std::string& value) {
    if (url.empty() || key.empty() || value.empty()) {
        return url;
    }
    const auto fragmentPos = url.find('#');
    const auto queryPos = url.find('?');
    const auto hasQuery = queryPos != std::string::npos && (fragmentPos == std::string::npos || queryPos < fragmentPos);
    const auto separator = hasQuery ? "&" : "?";
    const auto param = separator + urlEncode(key) + "=" + urlEncode(value);
    if (fragmentPos == std::string::npos) {
        return url + param;
    }
    return url.substr(0, fragmentPos) + param + url.substr(fragmentPos);
}

std::string applyCameraAuth(const std::string& url, const edge_gateway::CameraAuthConfig& auth) {
    if (!auth.enabled) {
        return url;
    }
    const auto mode = toLower(auth.mode);
    if (mode == "basic" || mode == "url_userinfo" || mode == "userinfo") {
        return insertUrlUserInfo(url, auth.username, auth.password);
    }
    if (mode == "token_query" || mode == "query_token") {
        return appendQueryParam(url, auth.tokenParam, auth.token);
    }
    return url;
}

std::string redactUrlCredentials(const std::string& url, const edge_gateway::CameraAuthConfig& auth) {
    std::string result = url;
    std::size_t searchPos = 0;
    while (true) {
        const auto schemeEnd = result.find("://", searchPos);
        if (schemeEnd == std::string::npos) {
            break;
        }
        const auto authorityStart = schemeEnd + 3;
        const auto authorityEnd = result.find('/', authorityStart);
        const auto hostEnd = authorityEnd == std::string::npos ? result.size() : authorityEnd;
        const auto at = result.find('@', authorityStart);
        if (at != std::string::npos && at < hostEnd) {
            result.replace(authorityStart, at - authorityStart + 1, "***:***@");
            searchPos = authorityStart + 8;
        } else {
            searchPos = authorityStart;
        }
    }
    if (auth.enabled && !auth.token.empty()) {
        const auto key = auth.tokenParam.empty() ? std::string("token") : auth.tokenParam;
        const auto marker = key + "=";
        auto pos = result.find(marker);
        while (pos != std::string::npos) {
            const auto valueStart = pos + marker.size();
            auto valueEnd = result.find_first_of("&#", valueStart);
            if (valueEnd == std::string::npos) {
                valueEnd = result.size();
            }
            result.replace(valueStart, valueEnd - valueStart, "***");
            pos = result.find(marker, valueStart + 3);
        }
    }
    return result;
}

std::string effectivePublishUrl(
    const edge_gateway::CameraServiceConfig& serviceConfig,
    const edge_gateway::CameraConfig& camera
) {
    std::string url;
    if (!camera.stream.publishUrl.empty()) {
        url = camera.stream.publishUrl;
    } else {
        url = joinUrl(serviceConfig.media.serverUrl, camera.stream.path);
    }
    return applyCameraAuth(url, serviceConfig.media.auth);
}

std::string buildFfmpegCommand(
    const edge_gateway::CameraServiceConfig& serviceConfig,
    const edge_gateway::CameraConfig& camera
) {
    if (!camera.command.empty()) {
        return camera.command;
    }

    const auto publishUrl = effectivePublishUrl(serviceConfig, camera);
    if (camera.source.empty() && camera.sourceType != "test") {
        throw std::invalid_argument("camera source is required: " + camera.cameraCode);
    }
    if (publishUrl.empty()) {
        throw std::invalid_argument("camera publishUrl or media.serverUrl/stream.path is required: " + camera.cameraCode);
    }

    std::ostringstream cmd;
    cmd << "ffmpeg -nostdin -hide_banner -loglevel warning ";
    if (camera.sourceType == "usb" || camera.sourceType == "uvc") {
        cmd << "-f v4l2 "
            << "-framerate " << camera.video.fps << " "
            << "-video_size " << camera.video.width << "x" << camera.video.height << " "
            << "-i " << shellQuote(camera.source) << " "
            << "-c:v libx264 -preset veryfast -tune zerolatency "
            << "-b:v " << camera.video.bitrateKbps << "k "
            << "-f rtsp " << shellQuote(publishUrl);
    } else if (camera.sourceType == "test") {
        cmd << "-re -f lavfi "
            << "-i " << shellQuote(
                "testsrc=size=" + std::to_string(camera.video.width) +
                "x" + std::to_string(camera.video.height) +
                ":rate=" + std::to_string(camera.video.fps)
            ) << " "
            << "-c:v libx264 -preset veryfast -tune zerolatency "
            << "-b:v " << camera.video.bitrateKbps << "k "
            << "-f rtsp " << shellQuote(publishUrl);
    } else {
        cmd << "-rtsp_transport " << shellQuote(serviceConfig.media.transport.empty() ? "tcp" : serviceConfig.media.transport) << " "
            << "-i " << shellQuote(applyCameraAuth(camera.source, camera.sourceAuth)) << " ";
        if (camera.video.codec == "copy") {
            cmd << "-c copy ";
        } else {
            cmd << "-c:v libx264 -preset veryfast -tune zerolatency "
                << "-b:v " << camera.video.bitrateKbps << "k ";
        }
        cmd << "-f rtsp " << shellQuote(publishUrl);
    }
    return cmd.str();
}

class ChildProcess {
public:
    bool start(const std::string& command) {
#ifdef _WIN32
        (void)command;
        return false;
#else
        if (running()) {
            return true;
        }
        const pid_t pid = fork();
        if (pid < 0) {
            lastError_ = std::strerror(errno);
            return false;
        }
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        pid_ = pid;
        return true;
#endif
    }

    bool running() {
#ifdef _WIN32
        return false;
#else
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0) {
            return true;
        }
        if (result == pid_) {
            if (WIFEXITED(status)) {
                lastExitCode_ = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                lastExitCode_ = 128 + WTERMSIG(status);
            }
            pid_ = -1;
        }
        return false;
#endif
    }

    void stop() {
#ifndef _WIN32
        if (pid_ <= 0) {
            return;
        }
        kill(pid_, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (!running()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(pid_, SIGKILL);
        while (running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }

    int lastExitCode() const {
        return lastExitCode_;
    }

    const std::string& lastError() const {
        return lastError_;
    }

private:
#ifndef _WIN32
    pid_t pid_ = -1;
#endif
    int lastExitCode_ = 0;
    std::string lastError_;
};

struct CameraRuntime {
    edge_gateway::CameraConfig config;
    std::string command;
    ChildProcess process;
    std::int64_t nextRestartMs = 0;
    bool lastOnline = false;
    int lastErrorCode = 0;
};

void registerStatusPoint(
    edge_gateway::MemoryPointStore& store,
    const std::string& machineCode,
    const edge_gateway::CameraConfig& camera,
    std::uint32_t index,
    const std::string& pointCode,
    const std::string& name,
    const std::string& dataType,
    std::int64_t ttlMs
) {
    if (index == 0) {
        return;
    }
    edge_gateway::PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.name = name;
    point.category = "camera";
    point.fullUpload = true;
    point.read.enable = true;
    point.read.dataType = dataType;
    point.read.cachePolicy.ttlMs = ttlMs;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.storeHistory = false;
    store.registerPoint(machineCode, camera.cameraCode, point);
}

void putStatusPoint(
    edge_gateway::MemoryPointStore& store,
    const std::string& machineCode,
    const edge_gateway::CameraConfig& camera,
    std::uint32_t index,
    const std::string& pointCode,
    const std::string& name,
    double value,
    int quality,
    std::int64_t ts,
    std::int64_t ttlMs
) {
    if (index == 0) {
        return;
    }
    edge_gateway::PointValue point;
    point.index = index;
    point.machineCode = machineCode;
    point.meterCode = camera.cameraCode;
    point.pointCode = pointCode;
    point.pointName = name;
    point.category = "camera";
    point.value = value;
    point.quality = quality;
    point.ts = ts;
    point.expireAt = ts + ttlMs;
    point.isStore = false;
    store.putLatest(point);
}

std::string cameraStatusPayload(
    const std::string& machineCode,
    const edge_gateway::CameraServiceConfig& serviceConfig,
    const CameraRuntime& camera,
    bool online,
    std::int64_t ts
) {
    const auto publishUrl = effectivePublishUrl(serviceConfig, camera.config);
    const auto statusUrl = serviceConfig.media.auth.hideCredentialsInStatus
        ? redactUrlCredentials(publishUrl, serviceConfig.media.auth)
        : publishUrl;
    std::ostringstream payload;
    payload << "{"
            << "\"machineCode\":\"" << jsonEscape(machineCode) << "\","
            << "\"cameraCode\":\"" << jsonEscape(camera.config.cameraCode) << "\","
            << "\"name\":\"" << jsonEscape(camera.config.name) << "\","
            << "\"online\":" << (online ? "true" : "false") << ","
            << "\"sourceType\":\"" << jsonEscape(camera.config.sourceType) << "\","
            << "\"fps\":" << (online ? camera.config.video.fps : 0) << ","
            << "\"bitrateKbps\":" << (online ? camera.config.video.bitrateKbps : 0) << ","
            << "\"streamUrl\":\"" << jsonEscape(statusUrl) << "\","
            << "\"errorCode\":" << camera.lastErrorCode << ","
            << "\"ts\":" << ts
            << "}";
    return payload.str();
}

class StdoutJsonPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
    void publishChangeEvent(const std::string&, const edge_gateway::StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const edge_gateway::OtaReply&) override {}
    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus&) override {}
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        std::cout << "camera status topic=" << topic << " payload=" << payload << std::endl;
    }
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override {
        return {};
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/camera-service.json";
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--once") {
            once = true;
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("camera-svc-" + sanitizeProcessToken(basenameOf(appConfigPath)));

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto machineCode = identity.machineCode.empty() ? appConfig.mqtt.clientId : identity.machineCode;

    auto& config = appConfig.cameraService;
    if (!config.enabled) {
        std::cout << "camera service disabled appConfig=" << appConfigPath << std::endl;
        return 0;
    }
    if (config.cameras.empty()) {
        std::cout << "camera service has no cameras appConfig=" << appConfigPath << std::endl;
        return 0;
    }
    if (config.sharedMemoryName.empty()) {
        config.sharedMemoryName = "gateway_point_store";
    }

    MemoryPointStore store(config.sharedMemoryName);
    const auto ttlMs = static_cast<std::int64_t>(config.statusIntervalMs) * 3;

    std::vector<CameraRuntime> cameras;
    for (const auto& cameraConfig : config.cameras) {
        if (!cameraConfig.enabled) {
            continue;
        }
        if (cameraConfig.cameraCode.empty()) {
            throw std::invalid_argument("cameraCode is required");
        }
        registerStatusPoint(
            store,
            machineCode,
            cameraConfig,
            cameraConfig.statusPointIndexes.online,
            "camera_online",
            cameraConfig.name + " 在线状态",
            "online_status",
            ttlMs
        );
        registerStatusPoint(
            store,
            machineCode,
            cameraConfig,
            cameraConfig.statusPointIndexes.fps,
            "camera_fps",
            cameraConfig.name + " 帧率",
            "double",
            ttlMs
        );
        registerStatusPoint(
            store,
            machineCode,
            cameraConfig,
            cameraConfig.statusPointIndexes.bitrateKbps,
            "camera_bitrate_kbps",
            cameraConfig.name + " 码率",
            "double",
            ttlMs
        );
        registerStatusPoint(
            store,
            machineCode,
            cameraConfig,
            cameraConfig.statusPointIndexes.errorCode,
            "camera_error_code",
            cameraConfig.name + " 错误码",
            "int32",
            ttlMs
        );

        CameraRuntime runtime;
        runtime.config = cameraConfig;
        runtime.command = buildFfmpegCommand(config, cameraConfig);
        cameras.push_back(runtime);
    }

    if (cameras.empty()) {
        std::cout << "camera service has no enabled cameras appConfig=" << appConfigPath << std::endl;
        return 0;
    }

    appConfig.mqtt.topicMachineCode = machineCode;
    if (!machineCode.empty()) {
        appConfig.mqtt.clientId = machineCode + "_camera_service";
    }
    std::shared_ptr<IMqttDriverPublisher> publisher;
    if (appConfig.mqtt.enabled) {
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(appConfig.mqtt);
    } else {
        publisher = std::make_shared<StdoutJsonPublisher>();
    }

    std::cout << "camera service started"
              << " appConfig=" << appConfigPath
              << " machineCode=" << machineCode
              << " cameras=" << cameras.size()
              << " shm=" << config.sharedMemoryName
              << " mqtt=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << std::endl;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    do {
        const auto ts = nowMs();
        for (auto& camera : cameras) {
            bool online = camera.process.running();
            if (!online && ts >= camera.nextRestartMs) {
                if (camera.process.start(camera.command)) {
                    auto safeCommand = redactUrlCredentials(camera.command, config.media.auth);
                    safeCommand = redactUrlCredentials(safeCommand, camera.config.sourceAuth);
                    std::cout << "camera stream started"
                              << " cameraCode=" << camera.config.cameraCode
                              << " command=" << safeCommand
                              << std::endl;
                    camera.lastErrorCode = 0;
                    camera.nextRestartMs = ts + config.media.reconnectIntervalMs;
                    online = true;
                } else {
                    camera.lastErrorCode = 127;
                    camera.nextRestartMs = ts + config.media.reconnectIntervalMs;
                    std::cerr << "camera stream start failed"
                              << " cameraCode=" << camera.config.cameraCode
                              << " error=" << camera.process.lastError()
                              << std::endl;
                }
            } else if (!online) {
                camera.lastErrorCode = camera.process.lastExitCode();
            }

            const int quality = online ? 1 : 0;
            putStatusPoint(
                store,
                machineCode,
                camera.config,
                camera.config.statusPointIndexes.online,
                "camera_online",
                camera.config.name + " 在线状态",
                online ? 1.0 : 0.0,
                quality,
                ts,
                ttlMs
            );
            putStatusPoint(
                store,
                machineCode,
                camera.config,
                camera.config.statusPointIndexes.fps,
                "camera_fps",
                camera.config.name + " 帧率",
                online ? camera.config.video.fps : 0.0,
                quality,
                ts,
                ttlMs
            );
            putStatusPoint(
                store,
                machineCode,
                camera.config,
                camera.config.statusPointIndexes.bitrateKbps,
                "camera_bitrate_kbps",
                camera.config.name + " 码率",
                online ? camera.config.video.bitrateKbps : 0.0,
                quality,
                ts,
                ttlMs
            );
            putStatusPoint(
                store,
                machineCode,
                camera.config,
                camera.config.statusPointIndexes.errorCode,
                "camera_error_code",
                camera.config.name + " 错误码",
                camera.lastErrorCode,
                quality,
                ts,
                ttlMs
            );

            publisher->publishJsonMessage(config.statusTopic, cameraStatusPayload(machineCode, config, camera, online, ts));
            camera.lastOnline = online;
        }
        if (once) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(config.statusIntervalMs));
    } while (g_running);

    for (auto& camera : cameras) {
        camera.process.stop();
    }

    std::cout << "camera service stopped" << std::endl;
    return 0;
}
