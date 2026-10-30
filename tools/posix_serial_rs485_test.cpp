#ifndef _WIN32

// transport.rs485: kernel RS-485 direction control is opt-in.
//  - disabled: the port is opened without any RS-485 ioctl, also on every reopen;
//  - enabled: read the driver settings (TIOCGRS485), apply ours keeping the bits we do not own
//    (TIOCSRS485), read back (TIOCGRS485) and compare every field we set. Each open does the whole
//    sequence again. Every failure (no RS-485 support, a refused or failed set, a failed read-back, a
//    driver that changes a field, also in place in the ioctl argument) is an explicit error naming the
//    device, the step and errno or the field, and the port is closed so its line lock is released.
// The ioctl interposer stands in for UART drivers; a pty is the real kernel without RS-485 support.
// None of this says how real hardware behaves; see doc/部署运维/RS-485方向逐串口核验.md.

#include <fcntl.h>
#include <linux/serial.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/posix_serial_port.hpp"

namespace {

// Fake UART driver. Kernel mode passes everything to the real kernel (a pty: no RS-485 support).
struct FakeDriver {
    bool kernel = true;
    serial_rs485 stored{};
    int getErrno = 0;             // first TIOCGRS485 of an open (reading the driver settings)
    int setErrno = 0;             // TIOCSRS485
    int readbackErrno = 0;        // second TIOCGRS485 of an open (read-back)
    std::function<void(serial_rs485&)> adjust;  // what the driver changes when applying
    bool adjustArgumentToo = false;              // the driver also copies its result into the argument
    int gets = 0;
    int sets = 0;
    std::vector<unsigned long> requests;
};

FakeDriver gDriver;

}  // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) __THROW {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    gDriver.requests.push_back(request);
    if (!gDriver.kernel && request == TIOCGRS485) {
        const int call = ++gDriver.gets;
        const int error = call % 2 == 1 ? gDriver.getErrno : gDriver.readbackErrno;
        if (error != 0) {
            errno = error;
            return -1;
        }
        *static_cast<serial_rs485*>(arg) = gDriver.stored;
        return 0;
    }
    if (!gDriver.kernel && request == TIOCSRS485) {
        ++gDriver.sets;
        if (gDriver.setErrno != 0) {
            errno = gDriver.setErrno;
            return -1;
        }
        auto* argument = static_cast<serial_rs485*>(arg);
        gDriver.stored = *argument;
        if (gDriver.adjust) gDriver.adjust(gDriver.stored);
        if (gDriver.adjustArgumentToo) *argument = gDriver.stored;
        return 0;
    }
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string gDevice;

void resetDriver(bool kernel) {
    gDriver = FakeDriver();
    gDriver.kernel = kernel;
}

bool issued(unsigned long request) {
    for (const auto r : gDriver.requests) {
        if (r == request) return true;
    }
    return false;
}

edge_gateway::SerialPortOptions plainOptions() {
    edge_gateway::SerialPortOptions options;
    options.device = gDevice;
    options.baudRate = 9600;
    return options;
}

edge_gateway::SerialPortOptions rs485Options() {
    auto options = plainOptions();
    options.rs485.enabled = true;
    options.rs485.rtsOnSend = true;
    options.rs485.rtsAfterSend = false;
    options.rs485.delayRtsBeforeSendMs = 2;
    options.rs485.delayRtsAfterSendMs = 3;
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

// After a failed open the descriptor is gone and the line lock is free for the next opener.
void requireReleased(edge_gateway::PosixSerialPort& port, const std::string& what) {
    require(!port.isOpen(), what + ": the port must not stay open");
    const auto saved = gDriver;
    gDriver.kernel = true;
    edge_gateway::PosixSerialPort next(plainOptions());
    next.open();
    require(next.isOpen(), what + ": the line lock must be released");
    next.close();
    gDriver = saved;
}

void requireFailure(const edge_gateway::SerialPortOptions& options, const std::vector<std::string>& parts,
                    const std::string& what) {
    edge_gateway::PosixSerialPort port(options);
    const auto error = openError(port);
    require(!error.empty(), what + ": open must fail");
    require(contains(error, gDevice), what + ": the error must name the device: " + error);
    for (const auto& part : parts) {
        require(contains(error, part), what + ": the error must mention '" + part + "': " + error);
    }
    requireReleased(port, what);
}

void disabledNeverTouchesRs485() {
    resetDriver(true);
    edge_gateway::PosixSerialPort port(plainOptions());
    port.open();
    port.close();
    port.open();
    require(port.isOpen(), "disabled rs485 opens normally");
    require(!issued(TIOCSRS485) && !issued(TIOCGRS485), "disabled rs485 must never issue an RS-485 ioctl, also on reopen");
}

void portWithoutRs485Support() {
    // A pty has no RS-485 at all: the very first read of the driver settings fails.
    resetDriver(true);
    requireFailure(rs485Options(), {"TIOCGRS485", std::strerror(ENOTTY), "errno " + std::to_string(ENOTTY)},
                   "pty without RS-485");
    require(!issued(TIOCSRS485), "settings must not be applied when they cannot be read");
    // A UART driver without rs485 support may still report zeros, then refuse the set.
    resetDriver(false);
    gDriver.setErrno = ENOTTY;
    requireFailure(rs485Options(), {"TIOCSRS485", std::strerror(ENOTTY)}, "driver refuses the set (ENOTTY)");
    resetDriver(false);
    gDriver.getErrno = EINVAL;
    requireFailure(rs485Options(), {"TIOCGRS485", std::strerror(EINVAL)}, "driver rejects the query (EINVAL)");
}

void setFails() {
    for (const int error : {EPERM, EIO, EINVAL}) {
        resetDriver(false);
        gDriver.setErrno = error;
        requireFailure(rs485Options(), {"TIOCSRS485", std::strerror(error), "errno " + std::to_string(error)},
                       "set fails with errno " + std::to_string(error));
    }
}

void readbackFails() {
    resetDriver(false);
    gDriver.readbackErrno = EIO;
    requireFailure(rs485Options(), {"TIOCGRS485", "reading the settings back", std::strerror(EIO)}, "read-back fails");
}

void readbackDiffers() {
    struct Case {
        const char* name;
        std::function<void(serial_rs485&)> adjust;
        const char* field;
    };
    const std::vector<Case> cases = {
        {"enable bit lost", [](serial_rs485& r) { r.flags &= ~SER_RS485_ENABLED; }, "RS-485 mode is off"},
        {"RTS while sending inverted", [](serial_rs485& r) { r.flags ^= SER_RS485_RTS_ON_SEND; }, "RTS while sending"},
        {"RTS after sending inverted", [](serial_rs485& r) { r.flags ^= SER_RS485_RTS_AFTER_SEND; }, "RTS after sending"},
        {"receive while sending forced", [](serial_rs485& r) { r.flags |= SER_RS485_RX_DURING_TX; }, "receive while sending"},
        {"delay before send dropped", [](serial_rs485& r) { r.delay_rts_before_send = 0; }, "RTS delay before send: requested 2 ms, driver 0 ms"},
        {"delay after send changed", [](serial_rs485& r) { r.delay_rts_after_send = 1; }, "RTS delay after send: requested 3 ms, driver 1 ms"},
    };
    for (const auto& item : cases) {
        for (const bool inPlace : {false, true}) {
            // inPlace: the driver also writes its result into the ioctl argument (as the kernel's
            // copy-back does); the comparison must still use the requested values.
            resetDriver(false);
            gDriver.adjust = item.adjust;
            gDriver.adjustArgumentToo = inPlace;
            requireFailure(rs485Options(), {"not applied", item.field},
                           std::string(item.name) + (inPlace ? " (in place)" : ""));
        }
    }
}

void appliedAndVerified() {
    resetDriver(false);
    gDriver.stored.flags = SER_RS485_TERMINATE_BUS | SER_RS485_RX_DURING_TX;  // set by the board/driver
    auto options = rs485Options();
    options.rs485.rtsOnSend = false;
    options.rs485.rtsAfterSend = true;
    edge_gateway::PosixSerialPort port(options);
    port.open();
    require(port.isOpen(), "settings taken by the driver open the port");
    require(gDriver.gets == 2 && gDriver.sets == 1, "read, apply and read back once per open");
    require(gDriver.stored.flags == (SER_RS485_ENABLED | SER_RS485_RTS_AFTER_SEND | SER_RS485_TERMINATE_BUS),
            "RTS levels follow the config, receive-while-sending follows localEcho=false, foreign bits are kept");
    require(gDriver.stored.delay_rts_before_send == 2 && gDriver.stored.delay_rts_after_send == 3, "delays follow the config");
    port.close();

    resetDriver(false);
    options.localEcho = true;
    edge_gateway::PosixSerialPort echo(options);
    echo.open();
    require((gDriver.stored.flags & SER_RS485_RX_DURING_TX) != 0,
            "with localEcho the receiver stays on while sending, so the echo can be checked");
}

void everyOpenAppliesAgain() {
    resetDriver(false);
    edge_gateway::PosixSerialPort port(rs485Options());
    port.open();
    port.close();  // e.g. a hard read/write error closes the port; the client reopens it
    gDriver.stored = serial_rs485{};  // the driver forgot the settings meanwhile
    port.open();
    require(gDriver.sets == 2 && gDriver.gets == 4, "a reopen must apply and verify the settings again");
    require((gDriver.stored.flags & SER_RS485_ENABLED) != 0, "a reopen restores RS-485 mode");
    port.close();

    // A failed open leaves nothing half-configured: the next open runs the full sequence again.
    resetDriver(false);
    gDriver.adjust = [](serial_rs485& r) { r.delay_rts_before_send = 0; };
    edge_gateway::PosixSerialPort retry(rs485Options());
    require(!openError(retry).empty(), "the first open fails on the read-back");
    gDriver.adjust = nullptr;
    retry.open();
    require(retry.isOpen() && gDriver.sets == 2, "the retry applies and verifies again and then opens");
}

}  // namespace

int main() {
    try {
        const int master = posix_openpt(O_RDWR | O_NOCTTY);
        require(master >= 0, "failed to create pseudo terminal");
        require(grantpt(master) == 0 && unlockpt(master) == 0, "failed to prepare pseudo terminal");
        const char* slaveName = ptsname(master);
        require(slaveName != nullptr, "failed to resolve pseudo terminal path");
        gDevice = slaveName;

        disabledNeverTouchesRs485();
        portWithoutRs485Support();
        setFails();
        readbackFails();
        readbackDiffers();
        appliedAndVerified();
        everyOpenAppliesAgain();

        close(master);
        std::cout << "posix serial rs485 test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "posix serial rs485 test failed: " << ex.what() << std::endl;
        return 1;
    }
}

#endif
