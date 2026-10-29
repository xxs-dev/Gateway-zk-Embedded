#include "edge_gateway/am5se_iec103_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void closeSocket(SocketHandle socketHandle) {
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

std::string temporaryDirectory(int port) {
    const char* base = std::getenv(
#ifdef _WIN32
        "TEMP"
#else
        "TMPDIR"
#endif
    );
    if (base == nullptr || *base == '\0') {
#ifdef _WIN32
        base = ".";
#else
        base = "/tmp";
#endif
    }
    std::string path(base);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "am5se-iec103-test-" + std::to_string(port);
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0775);
#endif
    return path;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

void removeTestOutput(const std::string& directory) {
    std::remove((directory + "/FAN00015.CFG").c_str());
    std::remove((directory + "/FAN00015.DAT").c_str());
#ifdef _WIN32
    _rmdir(directory.c_str());
#else
    rmdir(directory.c_str());
#endif
}

void sendAll(SocketHandle socketHandle, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto result = send(socketHandle, reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(bytes.size() - sent), 0);
#else
        const auto result = send(socketHandle, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (result <= 0) {
            throw std::runtime_error("mock send failed");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::uint8_t> receiveExact(SocketHandle socketHandle, std::size_t size) {
    std::vector<std::uint8_t> result(size);
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto count = recv(socketHandle, reinterpret_cast<char*>(result.data() + received),
            static_cast<int>(size - received), 0);
#else
        const auto count = recv(socketHandle, result.data() + received, size - received, 0);
#endif
        if (count <= 0) {
            throw std::runtime_error("mock receive failed");
        }
        received += static_cast<std::size_t>(count);
    }
    return result;
}

std::vector<std::uint8_t> receiveFrame(SocketHandle socketHandle) {
    auto first = receiveExact(socketHandle, 1);
    if (first[0] == 0x10) {
        auto rest = receiveExact(socketHandle, 4);
        first.insert(first.end(), rest.begin(), rest.end());
        return first;
    }
    if (first[0] != 0x68) {
        throw std::runtime_error("mock received invalid frame start");
    }
    auto header = receiveExact(socketHandle, 3);
    first.insert(first.end(), header.begin(), header.end());
    auto rest = receiveExact(socketHandle, static_cast<std::size_t>(first[1]) + 2U);
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

std::vector<std::uint8_t> variableFrame(const std::vector<std::uint8_t>& userData) {
    std::vector<std::uint8_t> frame = {
        0x68,
        static_cast<std::uint8_t>(userData.size()),
        static_cast<std::uint8_t>(userData.size()),
        0x68
    };
    frame.insert(frame.end(), userData.begin(), userData.end());
    std::uint32_t checksum = 0;
    for (const auto byte : userData) {
        checksum += byte;
    }
    frame.push_back(static_cast<std::uint8_t>(checksum & 0xFF));
    frame.push_back(0x16);
    return frame;
}

int availablePort() {
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("cannot allocate test socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socketHandle);
        throw std::runtime_error("cannot bind test socket");
    }
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closeSocket(socketHandle);
    return port;
}

void mockDevice(int port) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) != 0) {
        closeSocket(socketHandle);
        throw std::runtime_error("mock reverse TCP connect failed");
    }

    const std::vector<std::uint8_t> acknowledge = {0x10, 0x20, 0x01, 0x21, 0x16};
    receiveFrame(socketHandle);  // Reset communication.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // General interrogation.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // Class 1.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x2C, 0x01, 0x09, 0x01, 0x01, 0x64,
        0x01, 0x00, 0x00, 0x00, 0x00, 0xE9
    }));

    receiveFrame(socketHandle);  // Directory ASDU24.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // Class 1 directory response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x17, 0x01, 0x1F, 0x01, 0xFF, 0x00,
        0x0F, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x1A
    }));

    receiveFrame(socketHandle);  // CFG ASDU68.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // CFG class 1 response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x51,
        0x00, 0x80, 'C', 'F', 'G'
    }));

    receiveFrame(socketHandle);  // DAT ASDU68.
    sendAll(socketHandle, acknowledge);
    for (int packet = 0; packet < 12; ++packet) {
        if (packet == 1) {
            for (int dpi : {2, 1}) {
                receiveFrame(socketHandle);
                sendAll(socketHandle, variableFrame({0x28,1,1,0x81,1,1,1,100,
                    static_cast<std::uint8_t>(dpi),0x88,0x13,0,0,0}));
            }
        }
        receiveFrame(socketHandle);  // DAT class 1 response.
        sendAll(socketHandle, variableFrame({
            0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x52,
            static_cast<std::uint8_t>(packet),
            static_cast<std::uint8_t>(packet == 11 ? 0x80 : 0x00),
            static_cast<std::uint8_t>(packet), 0x02, 0x03
        }));
    }
    closeSocket(socketHandle);
}

void mockActiveDevice(int port) {
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        throw std::runtime_error("cannot create mock active TCP listener");
    }
    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        closeSocket(listener);
        throw std::runtime_error("cannot listen for mock active TCP client");
    }
    const auto socketHandle = accept(listener, nullptr, nullptr);
    closeSocket(listener);
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("mock active TCP accept failed");
    }

    const std::vector<std::uint8_t> acknowledge = {0x10, 0x20, 0x01, 0x21, 0x16};
    receiveFrame(socketHandle);  // Reset communication.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // General interrogation.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // Class 1.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x2C, 0x01, 0x09, 0x01, 0x01, 0x64,
        0x01, 0x00, 0x00, 0x00, 0x00, 0xE9
    }));

    receiveFrame(socketHandle);  // Class 2, ACD advertises an event.
    sendAll(socketHandle, acknowledge);
    const auto lostRequest = receiveFrame(socketHandle);
    const auto retryRequest = receiveFrame(socketHandle);
    requireTrue(lostRequest == retryRequest, "lost event response must retry identical FCB");
    sendAll(socketHandle, variableFrame({0x08,1,1,0x81,1,1,1,100,2,0x88,0x13,0,0,0}));
    // The sink fails once. A later poll must flush that event before this request.
    receiveFrame(socketHandle);
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);
    sendAll(socketHandle, variableFrame({0x08,1,1,0x81,1,1,1,100,1,0x89,0x13,0,0,0}));

    receiveFrame(socketHandle);  // Directory ASDU24.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // SOE interleaved in directory transfer.
    sendAll(socketHandle, variableFrame({0x28,1,1,0x81,1,1,1,100,2,0x90,0x13,0,0,0}));
    receiveFrame(socketHandle);  // Class 1 directory response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x17, 0x01, 0x1F, 0x01, 0xFF, 0x00,
        0x0F, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x1A
    }));

    receiveFrame(socketHandle);  // CFG ASDU68.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // CFG class 1 response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x51,
        0x00, 0x80, 'C', 'F', 'G'
    }));

    receiveFrame(socketHandle);  // DAT ASDU68.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // DAT class 1 response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x52,
        0x00, 0x80, 0x01, 0x02, 0x03
    }));
    closeSocket(socketHandle);
}

void testRecordingAssociation(int scenario) {
    using namespace edge_gateway;
    const int port=availablePort();std::exception_ptr error;
    std::thread device([&]{try{
        const auto listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        requireTrue(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener,1)==0,"association mock listen");
        const auto s=accept(listener,nullptr,nullptr);closeSocket(listener);
        const std::vector<std::uint8_t> ack={0x10,0x20,1,0x21,0x16};
        receiveFrame(s);sendAll(s,ack);receiveFrame(s);sendAll(s,ack);
        receiveFrame(s);sendAll(s,variableFrame({0x08,1,0x2c,1,9,1,1,100,1,0,0,0,0,0xe9}));
        auto directory=[&](bool recycled,bool partial){
            auto f=receiveFrame(s);requireTrue(f.size()>6&&f[6]==24,"directory uses existing connection");sendAll(s,ack);
            f=receiveFrame(s);requireTrue(f[0]==0x10&&(f[1]&15)==10,"class1 directory read");
            if(scenario==3){std::this_thread::sleep_for(std::chrono::milliseconds(750));return;}
            std::vector<std::uint8_t> reply={static_cast<std::uint8_t>(partial?0x28:0x08),1,23,1,31,1,255,0,
                15,0,6,0x88,0x13,0,0,static_cast<std::uint8_t>(recycled?30:29),10,26};
            if(scenario==4)reply[1]=2;
            if(scenario==5)reply.push_back(0);
            if(scenario==6)reply[5]=2;
            if(scenario==7)reply[4]|=0x40;
            sendAll(s,variableFrame(reply));
        };
        directory(false,scenario==2);
        if(scenario>=2){closeSocket(s);return;}
        auto f=receiveFrame(s);requireTrue(f[0]==0x10&&(f[1]&15)==11,"automatic directory refresh is throttled");
        sendAll(s,variableFrame({0x08,1,2,0x81,1,1,1,101,2,25,0,15,0,0x88,0x13,0,0}));
        directory(false,false);
        for(int kind:{0x51,0x52}){
            f=receiveFrame(s);requireTrue(f[6]==0x44,"only recording file request");sendAll(s,ack);
            receiveFrame(s);sendAll(s,variableFrame({0x08,1,0x50,0x81,0x14,1,1,static_cast<std::uint8_t>(kind),0,0x80,1,2,3}));
        }
        directory(scenario==1,false);closeSocket(s);
    }catch(...){error=std::current_exception();}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto output=temporaryDirectory(port);
    std::string cfg,dat;
    try{
        IecProtocolConfig c;c.transportMode="am5se_active_tcp";c.linkAddress=c.commonAddress=c.linkAddressSize=1;
        c.deviceFunctionType=1;c.interrogationCot=9;c.pollTimeoutMs=1000;c.maxPollFrames=scenario==2?1:8;
        c.soe.enabled=true;c.soe.recordingLinkEnabled=true;c.soe.recordingDirectoryTimeoutMs=500;c.soe.utcOffsetMinutes=0;
        TcpTransportConfig tcp;tcp.host="127.0.0.1";tcp.port=port;tcp.connectTimeoutMs=1000;
        Am5seIec103Client client(c,tcp);int snapshots=0,downloads=0;bool complete=false,success=true;
        client.setSoeSink([](const std::vector<Iec103SoeEvent>&){});
        client.setSoeRecordingSinks([&](const std::vector<Iec103DisturbanceRecord>&,bool done,bool ok,std::int64_t){++snapshots;complete=done;success=ok;},
            [&](const Iec103DisturbanceRecord& r,const Iec103ComtradeFiles& files,std::int64_t){requireTrue(r.fan==15&&fileExists(files.cfgPath)&&fileExists(files.datPath),"download callback only after files exist");++downloads;});
        const auto start=std::chrono::steady_clock::now();bool failed=false;
        try{client.poll();}catch(...){failed=true;}
        if(scenario>=4){
            requireTrue(failed&&!success&&!complete&&snapshots==1,"invalid directory cannot certify a complete catalogue");
        }else if(scenario==3){
            requireTrue(failed&&!success&&snapshots==1,"directory timeout surfaced without negative association");
            requireTrue(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(1000),"directory has a total deadline including retries");
        }else{
            requireTrue(!failed&&snapshots==1&&success,"automatic startup directory snapshot");
            requireTrue(complete==(scenario!=2),"directory completeness retained");
            if(scenario<2){
                client.poll();requireTrue(snapshots==1,"no repeated immediate automatic query");
                const auto files=client.pullComtradeRecording(15,output,1000);cfg=files.cfgPath;dat=files.datPath;
                requireTrue(snapshots==3,"download checks directory before and after");
                requireTrue(downloads==(scenario==0?1:0),"recycled FAN cannot attach downloaded files");
                requireTrue(cfg.find("FAN00015_")!=std::string::npos,"associated downloads use unique names");
            }
        }
    }catch(...){device.join();if(!cfg.empty())std::remove(cfg.c_str());if(!dat.empty())std::remove(dat.c_str());removeTestOutput(output);throw;}
    device.join();if(error)std::rethrow_exception(error);
    if(!cfg.empty())std::remove(cfg.c_str());if(!dat.empty())std::remove(dat.c_str());removeTestOutput(output);
}

}  // namespace

int main() {
    using namespace edge_gateway;

#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    const int port = availablePort();
    std::exception_ptr mockError;
    std::thread device([&] {
        try {
            mockDevice(port);
        } catch (...) {
            mockError = std::current_exception();
        }
    });

    const auto output = temporaryDirectory(port);
    removeTestOutput(output);
    try {
        IecProtocolConfig config;
        config.transportMode = "am5se_passive_tcp";
        config.listenAddress = "127.0.0.1";
        config.listenPort = port;
        config.udpBindAddress = "127.0.0.1";
        config.udpBroadcastAddress = "127.0.0.1";
        config.udpPort = port + 1;
        config.linkAddress = 1;
        config.commonAddress = 1;
        config.linkAddressSize = 1;
        config.deviceFunctionType = 1;
        config.interrogationCot = 9;
        config.pollTimeoutMs = 1000;
        config.t0Ms = 3000;
        config.maxPollFrames = 8;
        config.recordingMaxFileBytes = 1024;

        Am5seIec103Client client(config);
        std::vector<Iec103SoeEvent> soe;
        client.setSoeSink([&](const std::vector<Iec103SoeEvent>& events){soe.insert(soe.end(),events.begin(),events.end());});
        const auto values = client.poll();
        requireTrue(soe.empty(), "general interrogation must not create SOE");
        requireTrue(values.size() == 16, "passive TCP general interrogation values");
        requireTrue(values.front().informationNumber == 100 && values.front().value == 1.0,
            "passive TCP ASDU44 value");

        const auto records = client.listDisturbanceRecords(1000);
        requireTrue(records.size() == 1 && records.front().fan == 15, "passive TCP recording directory");

        const auto files = client.pullComtradeRecording(15, output, 1000);
        requireTrue(files.cfgBytes == 3 && files.datBytes == 36,
            "passive TCP COMTRADE should continue beyond maxPollFrames");
        requireTrue(fileExists(files.cfgPath) && fileExists(files.datPath),
            "passive TCP COMTRADE files");
        requireTrue(soe.size()==2 && soe[0].dpi==2 && soe[1].dpi==1 && soe[0].eventId!=soe[1].eventId,
            "SOE action/return during recording must be retained in order");
    } catch (...) {
        if (device.joinable()) {
            device.join();
        }
        removeTestOutput(output);
        throw;
    }
    device.join();
    if (mockError) {
        std::rethrow_exception(mockError);
    }
    removeTestOutput(output);

    const int activePort = availablePort();
    std::exception_ptr activeMockError;
    std::thread activeDevice([&] {
        try {
            mockActiveDevice(activePort);
        } catch (...) {
            activeMockError = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto activeOutput = temporaryDirectory(activePort);
    removeTestOutput(activeOutput);
    try {
        IecProtocolConfig config;
        config.transportMode = "am5se_active_tcp";
        config.linkAddress = 1;
        config.commonAddress = 1;
        config.linkAddressSize = 1;
        config.deviceFunctionType = 1;
        config.interrogationCot = 9;
        config.pollTimeoutMs = 1000;
        config.maxPollFrames = 8;
        config.recordingMaxFileBytes = 1024;
        TcpTransportConfig tcp;
        tcp.host = "127.0.0.1";
        tcp.port = activePort;
        tcp.connectTimeoutMs = 1000;

        Am5seIec103Client client(config, tcp);
        std::vector<Iec103SoeEvent> soe;
        std::string failedId;
        int sinkAttempts = 0;
        client.setSoeSink([&](const std::vector<Iec103SoeEvent>& events) {
            if (++sinkAttempts == 1) {
                failedId = events.at(0).eventId;
                throw std::runtime_error("injected storage failure");
            }
            if (sinkAttempts == 2) requireTrue(events.at(0).eventId == failedId, "pending SOE ID retained");
            soe.insert(soe.end(), events.begin(), events.end());
        });
        const auto values = client.poll();
        requireTrue(values.size() == 16, "active TCP general interrogation values");
        requireTrue(values.front().informationNumber == 100 && values.front().value == 1.0,
            "active TCP ASDU44 value");

        bool storageFailed = false;
        try { client.poll(); } catch (const std::runtime_error& error) {
            storageFailed = std::string(error.what()) == "injected storage failure";
        }
        requireTrue(storageFailed && soe.empty(), "storage failure surfaced");
        client.poll();
        requireTrue(soe.size() == 2 && soe[0].dpi == 2 && soe[1].dpi == 1,
            "ACD events and pending storage recovered in order");
        const auto records = client.listDisturbanceRecords(1000);
        requireTrue(records.size() == 1 && records.front().fan == 15, "active TCP recording directory");
        requireTrue(soe.size() == 3, "SOE during directory transfer retained");

        const auto files = client.pullComtradeRecording(15, activeOutput, 1000);
        requireTrue(files.cfgBytes == 3 && files.datBytes == 3, "active TCP COMTRADE sizes");
        requireTrue(fileExists(files.cfgPath) && fileExists(files.datPath),
            "active TCP COMTRADE files");
    } catch (...) {
        if (activeDevice.joinable()) {
            activeDevice.join();
        }
        removeTestOutput(activeOutput);
        throw;
    }
    activeDevice.join();
    if (activeMockError) {
        std::rethrow_exception(activeMockError);
    }
    removeTestOutput(activeOutput);
    for(int scenario=0;scenario<8;++scenario)testRecordingAssociation(scenario);
    std::cout << "AM5SE IEC103 passive and active TCP integration tests passed" << std::endl;
    return EXIT_SUCCESS;
}
