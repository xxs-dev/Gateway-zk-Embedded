#include "edge_gateway/am5se_iec103_client.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void closeSocket(SocketHandle socketHandle) {
    if (socketHandle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

void ensureSocketRuntime() {
#ifdef _WIN32
    static std::once_flag once;
    static int startupResult = 0;
    std::call_once(once, [] {
        WSADATA data{};
        startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startupResult != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

bool waitReadable(SocketHandle socketHandle, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socketHandle, &set);
    timeval timeout{};
    timeout.tv_sec = std::max(0, timeoutMs) / 1000;
    timeout.tv_usec = (std::max(0, timeoutMs) % 1000) * 1000;
#ifdef _WIN32
    const auto result = select(0, &set, nullptr, nullptr, &timeout);
#else
    const auto result = select(socketHandle + 1, &set, nullptr, nullptr, &timeout);
#endif
    return result > 0 && FD_ISSET(socketHandle, &set);
}

sockaddr_in ipv4Address(const std::string& address, int port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(static_cast<std::uint16_t>(port));
    const auto text = address.empty() ? std::string("0.0.0.0") : address;
    if (inet_pton(AF_INET, text.c_str(), &result.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + text);
    }
    return result;
}

std::string peerAddress(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr) {
        return {};
    }
    return text;
}

int makeDirectory(const std::string& path) {
#ifdef _WIN32
    return _mkdir(path.c_str());
#else
    return mkdir(path.c_str(), 0775);
#endif
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("IEC103 recording directory is empty");
    }
    std::string current;
    std::size_t cursor = 0;
    if (path.front() == '/' || path.front() == '\\') {
        current.assign(1, path.front());
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const auto next = path.find_first_of("/\\", cursor);
        const auto part = path.substr(
            cursor,
            next == std::string::npos ? std::string::npos : next - cursor);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/' && current.back() != '\\') {
                current.push_back('/');
            }
            current += part;
            if (makeDirectory(current) != 0 && errno != EEXIST) {
                throw std::runtime_error("cannot create IEC103 recording directory: " + current);
            }
        }
        if (next == std::string::npos) {
            break;
        }
        cursor = next + 1;
    }
}

std::string joinPath(const std::string& directory, const std::string& fileName) {
    if (directory.empty()) {
        return fileName;
    }
    if (directory.back() == '/' || directory.back() == '\\') {
        return directory + fileName;
    }
    return directory + "/" + fileName;
}

void writeBinaryFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot create IEC103 recording file: " + path);
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        throw std::runtime_error("cannot write IEC103 recording file: " + path);
    }
}

}  // namespace

Am5seIec103Client::Am5seIec103Client(IecProtocolConfig config)
    : config_(std::move(config)) {
    if (config_.transportMode != "am5se_passive_tcp") {
        throw std::invalid_argument("AM5SE IEC103 client requires transportMode=am5se_passive_tcp");
    }
}

Am5seIec103Client::~Am5seIec103Client() {
    disconnect();
    closeListener();
}

std::vector<IecDataValue> Am5seIec103Client::poll() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    ensureConnected();
    if (!generalInterrogationDone_) {
        auto values = performGeneralInterrogation(config_.pollTimeoutMs);
        generalInterrogationDone_ = true;
        return values;
    }

    auto frames = requestClassData(2, config_.pollTimeoutMs);
    auto values = decodeDataFrames(frames);
    bool accessDemand = std::any_of(frames.begin(), frames.end(), [](const auto& frame) {
        return IecCodec::ft12AccessDemand(frame);
    });
    for (int i = 0; accessDemand && i < config_.maxPollFrames; ++i) {
        frames = requestClassData(1, config_.pollTimeoutMs);
        const auto more = decodeDataFrames(frames);
        values.insert(values.end(), more.begin(), more.end());
        accessDemand = std::any_of(frames.begin(), frames.end(), [](const auto& frame) {
            return IecCodec::ft12AccessDemand(frame);
        });
    }
    return values;
}

std::vector<Iec103DisturbanceRecord> Am5seIec103Client::listDisturbanceRecords(int timeoutMs) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    ensureConnected();
    exchange(
        IecCodec::buildIec103RecordingDirectoryFrame(config_, nextFrameCountBit()),
        timeoutMs,
        1);

    std::vector<Iec103DisturbanceRecord> result;
    for (int i = 0; i < config_.maxPollFrames && result.size() < 50U; ++i) {
        const auto frames = requestClassData(1, timeoutMs);
        bool accessDemand = false;
        for (const auto& frame : frames) {
            accessDemand = accessDemand || IecCodec::ft12AccessDemand(frame);
            if (!IecCodec::isFt12VariableFrame(frame)) {
                continue;
            }
            const auto userData = IecCodec::ft12UserData(frame, config_.linkAddressSize);
            const auto records = IecCodec::decodeIec103RecordingDirectory(userData, config_);
            result.insert(result.end(), records.begin(), records.end());
        }
        if (!accessDemand) {
            break;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.fan < right.fan;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.fan == right.fan;
    }), result.end());
    return result;
}

Iec103ComtradeFiles Am5seIec103Client::pullComtradeRecording(
    int fan,
    const std::string& outputDirectory,
    int timeoutMs
) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    ensureConnected();
    if (recordingProgressCallback_) {
        recordingProgressCallback_("pulling_cfg");
    }
    const auto cfg = pullComtradeFile(fan, 0x51, timeoutMs);
    if (recordingProgressCallback_) {
        recordingProgressCallback_("pulling_dat");
    }
    const auto dat = pullComtradeFile(fan, 0x52, timeoutMs);

    const auto directory = outputDirectory.empty() ? config_.recordingDirectory : outputDirectory;
    ensureDirectory(directory);
    std::ostringstream baseName;
    baseName << "FAN" << std::setw(5) << std::setfill('0') << fan;
    const auto cfgPath = joinPath(directory, baseName.str() + ".CFG");
    const auto datPath = joinPath(directory, baseName.str() + ".DAT");
    const auto token = std::to_string(nowMs());
    const auto cfgTemp = joinPath(directory, baseName.str() + ".CFG." + token + ".tmp");
    const auto datTemp = joinPath(directory, baseName.str() + ".DAT." + token + ".tmp");

    try {
        writeBinaryFile(cfgTemp, cfg);
        writeBinaryFile(datTemp, dat);
        std::remove(cfgPath.c_str());
        std::remove(datPath.c_str());
        if (std::rename(cfgTemp.c_str(), cfgPath.c_str()) != 0) {
            throw std::runtime_error("cannot finalize IEC103 CFG recording: " + cfgPath);
        }
        if (std::rename(datTemp.c_str(), datPath.c_str()) != 0) {
            throw std::runtime_error("cannot finalize IEC103 DAT recording: " + datPath);
        }
    } catch (...) {
        std::remove(cfgTemp.c_str());
        std::remove(datTemp.c_str());
        throw;
    }

    Iec103ComtradeFiles result;
    result.fan = fan;
    result.cfgPath = cfgPath;
    result.datPath = datPath;
    result.cfgBytes = cfg.size();
    result.datBytes = dat.size();
    return result;
}

void Am5seIec103Client::setRecordingProgressCallback(
    std::function<void(const std::string&)> callback
) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    recordingProgressCallback_ = std::move(callback);
}

void Am5seIec103Client::ensureConnected() {
    sendDiscoveryIfDue();
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        acceptReverseConnection();
    }
    if (!linkInitialized_) {
        initializeLink();
    }
}

void Am5seIec103Client::ensureListener() {
    if (listenerSocket_ != static_cast<std::intptr_t>(kInvalidSocket)) {
        return;
    }
    ensureSocketRuntime();
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        throw std::runtime_error("AM5SE IEC103 TCP listener socket failed");
    }
    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    const auto address = ipv4Address(config_.listenAddress, config_.listenPort);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        closeSocket(listener);
        throw std::runtime_error(
            "AM5SE IEC103 cannot listen on " + config_.listenAddress + ":" +
            std::to_string(config_.listenPort));
    }
    listenerSocket_ = static_cast<std::intptr_t>(listener);
}

void Am5seIec103Client::acceptReverseConnection() {
    ensureListener();
    sendDiscoveryIfDue(true);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1, config_.t0Ms));
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (!waitReadable(static_cast<SocketHandle>(listenerSocket_), std::max(1, remaining))) {
            break;
        }
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLength = sizeof(peer);
#else
        socklen_t peerLength = sizeof(peer);
#endif
        const auto accepted = accept(
            static_cast<SocketHandle>(listenerSocket_),
            reinterpret_cast<sockaddr*>(&peer),
            &peerLength);
        if (accepted == kInvalidSocket) {
            continue;
        }
        const auto remoteIp = peerAddress(peer);
        if (!config_.allowedDeviceIp.empty() && remoteIp != config_.allowedDeviceIp) {
            closeSocket(accepted);
            continue;
        }
        int keepAlive = 1;
#ifdef _WIN32
        setsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
#else
        setsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
#endif
        socket_ = static_cast<std::intptr_t>(accepted);
        rxBuffer_.clear();
        linkInitialized_ = false;
        generalInterrogationDone_ = false;
        frameCountBit_ = false;
        return;
    }
    throw std::runtime_error(
        "waiting for AM5SE IEC103 reverse TCP connection on " + config_.listenAddress + ":" +
        std::to_string(config_.listenPort));
}

void Am5seIec103Client::initializeLink() {
    const auto frames = exchange(
        IecCodec::buildIec103ResetCommunicationFrame(config_),
        config_.pollTimeoutMs,
        1);
    const auto acknowledged = std::any_of(frames.begin(), frames.end(), [&](const auto& frame) {
        return IecCodec::isFt12FixedFrame(frame, config_.linkAddressSize) &&
            (IecCodec::ft12Control(frame) & 0x0FU) == 0;
    });
    if (!acknowledged) {
        throw std::runtime_error("AM5SE IEC103 reset communication was not acknowledged");
    }
    linkInitialized_ = true;
    frameCountBit_ = false;
}

void Am5seIec103Client::sendDiscoveryIfDue(bool force) {
    const auto timestamp = nowMs();
    const auto intervalMs = static_cast<std::int64_t>(config_.udpBroadcastIntervalSec) * 1000;
    if (!force && lastDiscoveryMs_ > 0 && timestamp - lastDiscoveryMs_ < intervalMs) {
        return;
    }
    ensureListener();
    const auto udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == kInvalidSocket) {
        throw std::runtime_error("AM5SE IEC103 UDP discovery socket failed");
    }
    int broadcast = 1;
#ifdef _WIN32
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
#else
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
#endif
    try {
        if (!config_.udpBindAddress.empty() && config_.udpBindAddress != "0.0.0.0") {
            const auto source = ipv4Address(config_.udpBindAddress, 0);
            if (bind(udp, reinterpret_cast<const sockaddr*>(&source), sizeof(source)) != 0) {
                throw std::runtime_error("AM5SE IEC103 UDP bind failed: " + config_.udpBindAddress);
            }
        }
        const auto destination = ipv4Address(config_.udpBroadcastAddress, config_.udpPort);
        const auto packet = IecCodec::buildAm5seUdpDiscoveryPacket(timestamp, config_.stationName);
#ifdef _WIN32
        const auto sent = sendto(
            udp,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
#else
        const auto sent = sendto(
            udp,
            packet.data(),
            packet.size(),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
#endif
        if (sent != static_cast<decltype(sent)>(packet.size())) {
            throw std::runtime_error("AM5SE IEC103 UDP discovery send failed");
        }
    } catch (...) {
        closeSocket(udp);
        throw;
    }
    closeSocket(udp);
    lastDiscoveryMs_ = timestamp;
}

void Am5seIec103Client::sendAll(const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto result = send(
            static_cast<SocketHandle>(socket_),
            reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(bytes.size() - sent),
            0);
#else
        const auto result = send(
            static_cast<SocketHandle>(socket_),
            bytes.data() + sent,
            bytes.size() - sent,
            0);
#endif
        if (result <= 0) {
            disconnect();
            throw std::runtime_error("AM5SE IEC103 TCP send failed");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::vector<std::uint8_t>> Am5seIec103Client::readFrames(int timeoutMs, int maxFrames) {
    std::vector<std::vector<std::uint8_t>> frames;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1, timeoutMs));
    while (std::chrono::steady_clock::now() < deadline &&
        static_cast<int>(frames.size()) < std::max(1, maxFrames)) {
        const auto remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        const int waitMs = frames.empty()
            ? std::max(1, remaining)
            : std::min(std::max(1, config_.idleReadTimeoutMs), std::max(1, remaining));
        if (!waitReadable(static_cast<SocketHandle>(socket_), waitMs)) {
            if (!frames.empty()) {
                break;
            }
            continue;
        }
        std::vector<std::uint8_t> chunk(1024);
#ifdef _WIN32
        const auto received = recv(
            static_cast<SocketHandle>(socket_),
            reinterpret_cast<char*>(chunk.data()),
            static_cast<int>(chunk.size()),
            0);
#else
        const auto received = recv(
            static_cast<SocketHandle>(socket_),
            chunk.data(),
            chunk.size(),
            0);
#endif
        if (received <= 0) {
            disconnect();
            throw std::runtime_error("AM5SE IEC103 TCP connection closed");
        }
        chunk.resize(static_cast<std::size_t>(received));
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());

        while (!rxBuffer_.empty() && static_cast<int>(frames.size()) < maxFrames) {
            const auto start = std::find_if(rxBuffer_.begin(), rxBuffer_.end(), [](std::uint8_t byte) {
                return byte == 0x10 || byte == 0x68;
            });
            rxBuffer_.erase(rxBuffer_.begin(), start);
            if (rxBuffer_.empty()) {
                break;
            }
            std::size_t frameSize = 0;
            if (rxBuffer_.front() == 0x10) {
                frameSize = static_cast<std::size_t>(4 + config_.linkAddressSize);
            } else {
                if (rxBuffer_.size() < 4) {
                    break;
                }
                if (rxBuffer_[1] != rxBuffer_[2] || rxBuffer_[3] != 0x68) {
                    rxBuffer_.erase(rxBuffer_.begin());
                    continue;
                }
                frameSize = static_cast<std::size_t>(rxBuffer_[1]) + 6U;
            }
            if (rxBuffer_.size() < frameSize) {
                break;
            }
            std::vector<std::uint8_t> frame(
                rxBuffer_.begin(),
                rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            rxBuffer_.erase(
                rxBuffer_.begin(),
                rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            const bool valid = frame.front() == 0x10
                ? IecCodec::isFt12FixedFrame(frame, config_.linkAddressSize)
                : IecCodec::isFt12VariableFrame(frame);
            if (valid) {
                frames.push_back(std::move(frame));
            }
        }
    }
    return frames;
}

std::vector<std::vector<std::uint8_t>> Am5seIec103Client::exchange(
    const std::vector<std::uint8_t>& request,
    int timeoutMs,
    int maxFrames
) {
    sendAll(request);
    auto frames = readFrames(timeoutMs, maxFrames);
    if (frames.empty()) {
        throw std::runtime_error("AM5SE IEC103 response timeout");
    }
    return frames;
}

std::vector<std::vector<std::uint8_t>> Am5seIec103Client::requestClassData(
    int dataClass,
    int timeoutMs
) {
    return exchange(
        IecCodec::buildIec103ClassRequestFrame(config_, dataClass, nextFrameCountBit()),
        timeoutMs,
        1);
}

std::vector<IecDataValue> Am5seIec103Client::decodeDataFrames(
    const std::vector<std::vector<std::uint8_t>>& frames
) const {
    std::vector<IecDataValue> values;
    for (const auto& frame : frames) {
        if (!IecCodec::isFt12VariableFrame(frame)) {
            continue;
        }
        const auto userData = IecCodec::ft12UserData(frame, config_.linkAddressSize);
        const auto decoded = IecCodec::decodeIec103Data(userData, config_);
        values.insert(values.end(), decoded.begin(), decoded.end());
    }
    return values;
}

std::vector<IecDataValue> Am5seIec103Client::performGeneralInterrogation(int timeoutMs) {
    exchange(IecCodec::buildIec103GeneralInterrogationFrame(config_), timeoutMs, 1);
    frameCountBit_ = true;
    std::vector<IecDataValue> values;
    for (int i = 0; i < config_.maxPollFrames; ++i) {
        const auto frames = requestClassData(1, timeoutMs);
        const auto decoded = decodeDataFrames(frames);
        values.insert(values.end(), decoded.begin(), decoded.end());
        bool completed = false;
        bool accessDemand = false;
        for (const auto& frame : frames) {
            accessDemand = accessDemand || IecCodec::ft12AccessDemand(frame);
            if (!IecCodec::isFt12VariableFrame(frame)) {
                continue;
            }
            const auto data = IecCodec::ft12UserData(frame, config_.linkAddressSize);
            const auto header = static_cast<std::size_t>(1 + config_.linkAddressSize);
            completed = completed || (data.size() > header && data[header] == 0x08);
        }
        if (completed || !accessDemand) {
            break;
        }
    }
    return values;
}

std::vector<std::uint8_t> Am5seIec103Client::pullComtradeFile(
    int fan,
    int fileType,
    int timeoutMs
) {
    exchange(
        IecCodec::buildIec103ComtradeFileCallFrame(config_, fan, fileType, nextFrameCountBit()),
        timeoutMs,
        1);
    Iec103ComtradeAssembler assembler(config_.recordingMaxFileBytes);
    for (int i = 0; i < config_.maxPollFrames && !assembler.complete(); ++i) {
        const auto frames = requestClassData(1, timeoutMs);
        for (const auto& frame : frames) {
            if (!IecCodec::isFt12VariableFrame(frame)) {
                continue;
            }
            const auto userData = IecCodec::ft12UserData(frame, config_.linkAddressSize);
            const auto header = static_cast<std::size_t>(1 + config_.linkAddressSize);
            if (userData.size() > header && userData[header] == 0x50) {
                assembler.add(IecCodec::decodeIec103ComtradeChunk(userData, config_));
            }
        }
    }
    if (!assembler.complete()) {
        throw std::runtime_error(
            std::string("IEC103 COMTRADE ") + (fileType == 0x51 ? "CFG" : "DAT") +
            " transfer ended without final packet");
    }
    return assembler.bytes();
}

bool Am5seIec103Client::nextFrameCountBit() {
    frameCountBit_ = !frameCountBit_;
    return frameCountBit_;
}

void Am5seIec103Client::disconnect() {
    closeSocket(static_cast<SocketHandle>(socket_));
    socket_ = static_cast<std::intptr_t>(kInvalidSocket);
    rxBuffer_.clear();
    linkInitialized_ = false;
    generalInterrogationDone_ = false;
}

void Am5seIec103Client::closeListener() {
    closeSocket(static_cast<SocketHandle>(listenerSocket_));
    listenerSocket_ = static_cast<std::intptr_t>(kInvalidSocket);
}

}  // namespace edge_gateway
