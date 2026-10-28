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
#include <thread>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
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

constexpr int kClass2DuringTransferMs = 1000;
constexpr std::size_t kMaxBufferedValues = 4096;

std::int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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
#ifdef _WIN32
    if (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        current = path.substr(0, 3);
        cursor = 3;
    } else
#endif
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

Am5seIec103Client::Am5seIec103Client(IecProtocolConfig config, TcpTransportConfig tcp)
    : config_(std::move(config)),
      tcp_(std::move(tcp)),
      master_(ft12LinkOptions(Ft12Profile::Iec103, config_)),
      session_(Ft12Profile::Iec103, config_, master_) {
    if (config_.transportMode != "am5se_passive_tcp" &&
        config_.transportMode != "am5se_active_tcp") {
        throw std::invalid_argument(
            "AM5SE IEC103 client requires transportMode=am5se_passive_tcp or am5se_active_tcp");
    }
    if (config_.transportMode == "am5se_active_tcp" &&
        (tcp_.host.empty() || tcp_.port <= 0 || tcp_.port > 65535)) {
        throw std::invalid_argument("AM5SE IEC103 active TCP requires protocol.tcp.host and port");
    }
}

Am5seIec103Client::~Am5seIec103Client() {
    disconnect();
    closeListener();
}

std::vector<IecDataValue> Am5seIec103Client::poll() {
    // A recording transfer holds the link for its whole duration; it keeps requesting class 1
    // (and periodically class 2) data and buffers every event for drainBufferedValues().
    std::unique_lock<std::mutex> lock(operationMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {};
    }
    try {
        ensureConnected();
        return session_.poll();
    } catch (const Ft12TransportError&) {
        disconnect();
        throw;
    }
}

std::vector<IecDataValue> Am5seIec103Client::drainBufferedValues() {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    std::vector<IecDataValue> values;
    values.swap(bufferedValues_);
    return values;
}

std::vector<Iec103DisturbanceRecord> Am5seIec103Client::listDisturbanceRecords(int timeoutMs) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    try {
        ensureConnected();
        session_.ensureLink();
        const auto deadline = steadyNowMs() + std::max(1, timeoutMs);
        sendRecordingRequest(IecCodec::buildIec103RecordingDirectoryAsdu(config_), "recording directory request");
        std::vector<Iec103DisturbanceRecord> result;
        for (int i = 0; i < config_.maxPollFrames && result.size() < 50U && steadyNowMs() < deadline; ++i) {
            const auto reply = master_.requestClassData(1);
            if (reply.hasUserData() && reply.asdu[0] == 0x17) {
                const auto records = IecCodec::decodeIec103RecordingDirectory(reply.asdu);
                result.insert(result.end(), records.begin(), records.end());
            } else {
                keepEvents(reply);
            }
            if (!reply.accessDemand) {
                break;
            }
        }
        std::sort(result.begin(), result.end(), [](const Iec103DisturbanceRecord& left, const Iec103DisturbanceRecord& right) {
            return left.fan < right.fan;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const Iec103DisturbanceRecord& left, const Iec103DisturbanceRecord& right) {
            return left.fan == right.fan;
        }), result.end());
        return result;
    } catch (const Ft12TransportError&) {
        disconnect();
        throw;
    }
}

Iec103ComtradeFiles Am5seIec103Client::pullComtradeRecording(
    int fan,
    const std::string& outputDirectory,
    int timeoutMs
) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    std::vector<std::uint8_t> cfg;
    std::vector<std::uint8_t> dat;
    try {
        ensureConnected();
        session_.ensureLink();
        const auto deadline = steadyNowMs() + std::max(1, timeoutMs);
        if (recordingProgressCallback_) {
            recordingProgressCallback_("pulling_cfg");
        }
        cfg = pullComtradeFile(fan, 0x51, deadline);
        if (recordingProgressCallback_) {
            recordingProgressCallback_("pulling_dat");
        }
        dat = pullComtradeFile(fan, 0x52, deadline);
    } catch (const Ft12TransportError&) {
        disconnect();
        throw;
    }

    const auto directory = outputDirectory.empty() ? config_.recordingDirectory : outputDirectory;
    ensureDirectory(directory);
    std::ostringstream baseName;
    baseName << "FAN" << std::setw(5) << std::setfill('0') << fan;
    const auto cfgPath = joinPath(directory, baseName.str() + ".CFG");
    const auto datPath = joinPath(directory, baseName.str() + ".DAT");
    const auto token = std::to_string(wallClockMs());
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
    if (transport_) {
        return;
    }
    if (config_.transportMode == "am5se_active_tcp") {
        ensureSocketRuntime();
        transport_ = SocketFt12Transport::connect(tcp_.host, tcp_.port, tcp_.connectTimeoutMs);
    } else {
        sendDiscoveryIfDue();
        acceptReverseConnection();
    }
    master_.attach(transport_.get());
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
        transport_.reset(new SocketFt12Transport(static_cast<std::intptr_t>(accepted)));
        return;
    }
    throw std::runtime_error(
        "waiting for AM5SE IEC103 reverse TCP connection on " + config_.listenAddress + ":" +
        std::to_string(config_.listenPort));
}

void Am5seIec103Client::sendDiscoveryIfDue(bool force) {
    const auto now = steadyNowMs();
    const auto intervalMs = static_cast<std::int64_t>(config_.udpBroadcastIntervalSec) * 1000;
    if (!force && lastDiscoveryMs_ > 0 && now - lastDiscoveryMs_ < intervalMs) {
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
        // The broadcast doubles as the station clock source: it carries wall-clock time.
        const auto packet = IecCodec::buildAm5seUdpDiscoveryPacket(wallClockMs(), config_.stationName);
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
    lastDiscoveryMs_ = now;
}

void Am5seIec103Client::sendRecordingRequest(const std::vector<std::uint8_t>& asdu, const char* what) {
    const auto reply = master_.sendConfirm(asdu);
    if (reply.nack()) {
        throw std::runtime_error(std::string("AM5SE IEC103 ") + what + " not accepted (NACK)");
    }
}

void Am5seIec103Client::keepEvents(const Ft12Reply& reply) {
    if (!reply.hasUserData()) {
        return;
    }
    std::vector<IecDataValue> values;
    session_.absorb(reply.asdu, values);
    if (values.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(bufferMutex_);
    bufferedValues_.insert(bufferedValues_.end(), values.begin(), values.end());
    if (bufferedValues_.size() > kMaxBufferedValues) {
        bufferedValues_.erase(
            bufferedValues_.begin(),
            bufferedValues_.begin() + static_cast<std::ptrdiff_t>(bufferedValues_.size() - kMaxBufferedValues));
    }
}

void Am5seIec103Client::pollClass2IfDue(std::int64_t& lastClass2Ms) {
    const auto now = steadyNowMs();
    if (now - lastClass2Ms < std::max(kClass2DuringTransferMs, config_.pollTimeoutMs)) {
        return;
    }
    lastClass2Ms = now;
    keepEvents(master_.requestClassData(2));
}

std::vector<std::uint8_t> Am5seIec103Client::pullComtradeFile(int fan, int fileType, std::int64_t deadlineMs) {
    sendRecordingRequest(IecCodec::buildIec103ComtradeFileCallAsdu(config_, fan, fileType), "COMTRADE file call");
    Iec103ComtradeAssembler assembler(config_.recordingMaxFileBytes);
    std::int64_t lastClass2Ms = steadyNowMs();
    // The total deadline bounds the transfer: a DAT file can need far more class 1 requests
    // than maxPollFrames allows for a normal poll.
    while (!assembler.complete() && steadyNowMs() < deadlineMs) {
        pollClass2IfDue(lastClass2Ms);
        const auto reply = master_.requestClassData(1);
        if (reply.hasUserData() && reply.asdu[0] == 0x50) {
            assembler.add(IecCodec::decodeIec103ComtradeChunk(reply.asdu));
        } else if (reply.hasUserData()) {
            keepEvents(reply);
        } else if (!reply.accessDemand) {
            // Station is still preparing the next packet.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!assembler.complete()) {
        throw std::runtime_error(
            std::string("IEC103 COMTRADE ") + (fileType == 0x51 ? "CFG" : "DAT") +
            " transfer ended without final packet");
    }
    return assembler.bytes();
}

void Am5seIec103Client::disconnect() {
    master_.detach();
    transport_.reset();
}

void Am5seIec103Client::closeListener() {
    closeSocket(static_cast<SocketHandle>(listenerSocket_));
    listenerSocket_ = static_cast<std::intptr_t>(kInvalidSocket);
}

}  // namespace edge_gateway
