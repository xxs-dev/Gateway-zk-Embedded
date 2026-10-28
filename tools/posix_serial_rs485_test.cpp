#ifndef _WIN32

// transport.rs485: kernel RS-485 direction control is opt-in. Disabled → the port is opened without
// any RS-485 ioctl; enabled → TIOCSRS485 then a TIOCGRS485 read-back, and every failure (driver
// without RS-485 support, driver that adjusts a field) is an explicit error naming the device.

#include <fcntl.h>
#include <linux/serial.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/posix_serial_port.hpp"

namespace {

enum class DriverMode { Kernel, AcceptAll, DropsDelays };

DriverMode gMode = DriverMode::Kernel;
std::vector<unsigned long> gRequests;
serial_rs485 gStored{};

}  // namespace

// Interpose ioctl so the test can see which requests the port issues and stand in for a UART
// driver with RS-485 support (a pty has none). Everything else goes to the kernel.
extern "C" int ioctl(int fd, unsigned long request, ...) __THROW {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    gRequests.push_back(request);
    if (gMode != DriverMode::Kernel && request == TIOCSRS485) {
        gStored = *static_cast<serial_rs485*>(arg);
        if (gMode == DriverMode::DropsDelays) {
            gStored.delay_rts_before_send = 0;
            gStored.delay_rts_after_send = 0;
        }
        return 0;
    }
    if (gMode != DriverMode::Kernel && request == TIOCGRS485) {
        *static_cast<serial_rs485*>(arg) = gStored;
        return 0;
    }
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool issued(unsigned long request) {
    for (const auto r : gRequests) {
        if (r == request) return true;
    }
    return false;
}

edge_gateway::SerialPortOptions optionsFor(const std::string& device) {
    edge_gateway::SerialPortOptions options;
    options.device = device;
    options.baudRate = 9600;
    return options;
}

std::string openError(edge_gateway::PosixSerialPort& port) {
    try {
        port.open();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    return {};
}

void disabledIssuesNoRs485Ioctl(const std::string& device) {
    gMode = DriverMode::Kernel;
    gRequests.clear();
    edge_gateway::PosixSerialPort port(optionsFor(device));
    port.open();
    require(port.isOpen(), "disabled rs485 must open normally");
    require(!issued(TIOCSRS485) && !issued(TIOCGRS485), "disabled rs485 must not issue any RS-485 ioctl");
}

void enabledOnUnsupportedPortFails(const std::string& device) {
    gMode = DriverMode::Kernel;
    gRequests.clear();
    auto options = optionsFor(device);
    options.rs485.enabled = true;
    edge_gateway::PosixSerialPort port(options);
    const auto error = openError(port);
    require(issued(TIOCSRS485), "enabled rs485 must call TIOCSRS485");
    require(error.find("TIOCSRS485") != std::string::npos, "error must name the failed ioctl: " + error);
    require(error.find(device) != std::string::npos, "error must name the device: " + error);
    require(error.find(std::strerror(ENOTTY)) != std::string::npos && error.find("errno") != std::string::npos,
            "error must carry the errno: " + error);
    require(!port.isOpen(), "a port whose RS-485 setup failed must not stay open");
    // The lock is released with the descriptor: the line can be opened again.
    edge_gateway::PosixSerialPort again(optionsFor(device));
    again.open();
    require(again.isOpen(), "the failed open must release the line lock");
}

void enabledAppliesAndVerifiesSettings(const std::string& device) {
    gMode = DriverMode::AcceptAll;
    gRequests.clear();
    gStored = serial_rs485{};
    auto options = optionsFor(device);
    options.rs485.enabled = true;
    options.rs485.rtsOnSend = false;
    options.rs485.rtsAfterSend = true;
    options.rs485.delayRtsBeforeSendMs = 2;
    options.rs485.delayRtsAfterSendMs = 3;
    edge_gateway::PosixSerialPort port(options);
    port.open();
    require(port.isOpen(), "rs485 accepted by the driver must open");
    require(issued(TIOCGRS485), "enabled rs485 must read the settings back");
    require(gStored.flags == (SER_RS485_ENABLED | SER_RS485_RTS_AFTER_SEND), "RTS levels must follow the config");
    require(gStored.delay_rts_before_send == 2 && gStored.delay_rts_after_send == 3, "RTS delays must follow the config");
}

void driverAdjustingSettingsFails(const std::string& device) {
    gMode = DriverMode::DropsDelays;
    gRequests.clear();
    auto options = optionsFor(device);
    options.rs485.enabled = true;
    options.rs485.delayRtsAfterSendMs = 5;
    edge_gateway::PosixSerialPort port(options);
    const auto error = openError(port);
    require(error.find("not applied") != std::string::npos && error.find(device) != std::string::npos,
            "a driver that changes the RS-485 settings must be reported with the device: " + error);
    require(!port.isOpen(), "a port with unverified RS-485 settings must not stay open");
}

}  // namespace

int main() {
    try {
        const int master = posix_openpt(O_RDWR | O_NOCTTY);
        require(master >= 0, "failed to create pseudo terminal");
        require(grantpt(master) == 0 && unlockpt(master) == 0, "failed to prepare pseudo terminal");
        const char* slaveName = ptsname(master);
        require(slaveName != nullptr, "failed to resolve pseudo terminal path");
        const std::string device = slaveName;

        disabledIssuesNoRs485Ioctl(device);
        enabledOnUnsupportedPortFails(device);
        enabledAppliesAndVerifiesSettings(device);
        driverAdjustingSettingsFails(device);

        close(master);
        std::cout << "posix serial rs485 test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "posix serial rs485 test failed: " << ex.what() << std::endl;
        return 1;
    }
}

#endif
