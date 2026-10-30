#ifndef _WIN32

#include "edge_gateway/posix_serial_port.hpp"
#include "edge_gateway/channel_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <linux/serial.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace edge_gateway {

PosixSerialPort::PosixSerialPort(SerialPortOptions options) : options_(std::move(options)) {
}

PosixSerialPort::~PosixSerialPort() {
    close();
}

void PosixSerialPort::open() {
    if (fd_ >= 0) {
        return;
    }

    // Resolve the baud rate before opening: an unsupported rate must not leave
    // an open descriptor with unconfigured (canonical/echo) termios behind,
    // which a later open() would silently reuse.
    const auto baud = baudToTermios(options_.baudRate);

    fd_ = ::open(options_.device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd_ < 0) {
        const auto err = errno;
        if (err == EBUSY) {
            throw std::runtime_error("serial device busy (opened exclusively by another process): " +
                                     options_.device);
        }
        throw std::runtime_error("failed to open serial device: " + options_.device + ": " + std::strerror(err));
    }
    // One owner per line: two configurations on the same port would overwrite each other's line
    // settings and interleave frames. flock() covers every gateway process (also root); TIOCEXCL
    // additionally refuses other unprivileged openers while we hold the port.
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        const auto err = errno;
        ::close(fd_);
        fd_ = -1;
        if (err == EWOULDBLOCK) {
            throw std::runtime_error("serial device busy (locked by another driver or tool): " + options_.device);
        }
        throw std::system_error(err, std::generic_category(), "serial device lock failed: " + options_.device);
    }
    (void)ioctl(fd_, TIOCEXCL);

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        const auto err = errno;
        close();
        throw std::system_error(err, std::generic_category(), "tcgetattr failed");
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= options_.dataBits == 7 ? CS7 : CS8;

    if (options_.stopBits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_iflag &= ~(INPCK | IGNPAR | PARMRK);
    if (options_.parity == "E") {
        tty.c_cflag |= PARENB;
    } else if (options_.parity == "O") {
        tty.c_cflag |= PARENB | PARODD;
    }
    if (options_.parity == "E" || options_.parity == "O") {
        // Check received parity and drop characters with parity/framing errors, so a corrupted byte
        // shortens the frame (rejected by length/CRC) instead of passing on as data.
        tty.c_iflag |= INPCK | IGNPAR;
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        const auto err = errno;
        close();
        throw std::system_error(err, std::generic_category(), "tcsetattr failed");
    }
    if (options_.rs485.enabled) {
        applyRs485();
    }
    // Drop bytes received before the port was configured.
    (void)tcflush(fd_, TCIFLUSH);
}

namespace {

// Bits this driver owns. Everything else in serial_rs485.flags (bus termination, addressing, ...) is
// read from the driver and written back unchanged.
constexpr std::uint32_t kRs485ControlledFlags =
    SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND | SER_RS485_RX_DURING_TX;

std::string rtsLevel(bool asserted) {
    return asserted ? "asserted" : "deasserted";
}

// Every field the driver did not take as requested, in words.
std::string rs485Differences(const serial_rs485& requested, const serial_rs485& actual) {
    std::string out;
    const auto add = [&out](const std::string& text) { out += (out.empty() ? "" : "; ") + text; };
    const auto bit = [](const serial_rs485& value, std::uint32_t flag) { return (value.flags & flag) != 0; };
    if (bit(requested, SER_RS485_ENABLED) != bit(actual, SER_RS485_ENABLED)) {
        add("RS-485 mode is off in the driver");
    }
    if (bit(requested, SER_RS485_RTS_ON_SEND) != bit(actual, SER_RS485_RTS_ON_SEND)) {
        add("RTS while sending: requested " + rtsLevel(bit(requested, SER_RS485_RTS_ON_SEND)) + ", driver " +
            rtsLevel(bit(actual, SER_RS485_RTS_ON_SEND)));
    }
    if (bit(requested, SER_RS485_RTS_AFTER_SEND) != bit(actual, SER_RS485_RTS_AFTER_SEND)) {
        add("RTS after sending: requested " + rtsLevel(bit(requested, SER_RS485_RTS_AFTER_SEND)) + ", driver " +
            rtsLevel(bit(actual, SER_RS485_RTS_AFTER_SEND)));
    }
    if (bit(requested, SER_RS485_RX_DURING_TX) != bit(actual, SER_RS485_RX_DURING_TX)) {
        add(std::string("receive while sending: requested ") + (bit(requested, SER_RS485_RX_DURING_TX) ? "on" : "off") +
            ", driver " + (bit(actual, SER_RS485_RX_DURING_TX) ? "on" : "off"));
    }
    if (requested.delay_rts_before_send != actual.delay_rts_before_send) {
        add("RTS delay before send: requested " + std::to_string(requested.delay_rts_before_send) + " ms, driver " +
            std::to_string(actual.delay_rts_before_send) + " ms");
    }
    if (requested.delay_rts_after_send != actual.delay_rts_after_send) {
        add("RTS delay after send: requested " + std::to_string(requested.delay_rts_after_send) + " ms, driver " +
            std::to_string(actual.delay_rts_after_send) + " ms");
    }
    return out;
}

}  // namespace

void PosixSerialPort::applyRs485() {
    // Hand the transceiver direction to the UART driver on every open (also every reopen after an error),
    // then read the settings back: a driver that lacks RS-485 support or silently adjusts a field would
    // otherwise leave the line half-working. Any failure closes the port, which releases the line lock.
    const auto fail = [this](const std::string& what) {
        const auto err = errno;
        close();
        throw std::runtime_error("RS-485 " + what + " failed on " + options_.device + ": " + std::strerror(err) +
                                 " (errno " + std::to_string(err) + ")");
    };
    serial_rs485 current{};
    if (ioctl(fd_, TIOCGRS485, &current) != 0) fail("TIOCGRS485 (reading the driver settings)");

    const auto& wanted = options_.rs485;
    serial_rs485 requested = current;
    requested.flags &= ~kRs485ControlledFlags;
    requested.flags |= SER_RS485_ENABLED;
    if (wanted.rtsOnSend) requested.flags |= SER_RS485_RTS_ON_SEND;
    if (wanted.rtsAfterSend) requested.flags |= SER_RS485_RTS_AFTER_SEND;
    // A transceiver that echoes the sent bytes (transport.localEcho) must keep its receiver on while
    // sending, so the echo is read and compared; otherwise the kernel gates the receiver during sending.
    if (options_.localEcho) requested.flags |= SER_RS485_RX_DURING_TX;
    requested.delay_rts_before_send = static_cast<std::uint32_t>(wanted.delayRtsBeforeSendMs);
    requested.delay_rts_after_send = static_cast<std::uint32_t>(wanted.delayRtsAfterSendMs);

    // The kernel may sanitize the argument in place; compare against an untouched copy.
    serial_rs485 argument = requested;
    if (ioctl(fd_, TIOCSRS485, &argument) != 0) fail("TIOCSRS485 (applying the settings)");
    serial_rs485 actual{};
    if (ioctl(fd_, TIOCGRS485, &actual) != 0) fail("TIOCGRS485 (reading the settings back)");
    const auto differences = rs485Differences(requested, actual);
    if (!differences.empty()) {
        close();
        throw std::runtime_error("RS-485 settings not applied on " + options_.device + ": " + differences);
    }
}

void PosixSerialPort::close() {
    if (fd_ >= 0) {
        (void)ioctl(fd_, TIOCNXCL);
        ::close(fd_);
        fd_ = -1;
    }
}

bool PosixSerialPort::isOpen() const {
    return fd_ >= 0;
}

void PosixSerialPort::write(const std::vector<std::uint8_t>& bytes) {
    write(bytes, BeforePhysicalWrite());
}

void PosixSerialPort::write(const std::vector<std::uint8_t>& bytes, const BeforePhysicalWrite& beforeWrite) {
    if (fd_ < 0) {
        throw std::runtime_error("serial port is not open");
    }

    std::size_t written = 0;
    const auto check = [&] {
        try {
            if (beforeWrite) beforeWrite();
        } catch (...) {
            // Discard queued output; bytes already on the wire cannot be recalled.
            (void)tcflush(fd_, TCOFLUSH);
            close();
            throw;
        }
    };
    while (written < bytes.size()) {
        check();
        const auto rc = ::write(fd_, bytes.data() + written, bytes.size() - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto error = errno;
            close();
            throw std::system_error(error, std::generic_category(), "serial write failed");
        }
        if (rc == 0) {
            close();
            throw std::runtime_error("serial write returned zero bytes");
        }
        traceBytes(true, bytes.data() + written, static_cast<std::size_t>(rc));
        written += static_cast<std::size_t>(rc);
    }
    check();
    if (tcdrain(fd_) != 0) {
        const auto error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "serial drain failed");
    }
}

std::vector<std::uint8_t> PosixSerialPort::read(std::size_t maxBytes, int timeoutMs) {
    if (fd_ < 0) {
        throw std::runtime_error("serial port is not open");
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(0, timeoutMs));
    while (true) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd_, &readSet);

        const auto now = std::chrono::steady_clock::now();
        const auto remainingUs = std::max<std::int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count()
        );
        timeval tv{};
        tv.tv_sec = static_cast<long>(remainingUs / 1000000);
        tv.tv_usec = static_cast<long>(remainingUs % 1000000);
        const auto ready = select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR && std::chrono::steady_clock::now() < deadline) {
                continue;
            }
            if (errno == EINTR) {
                return {};
            }
            const auto error = errno;
            close();
            throw std::system_error(error, std::generic_category(), "select failed");
        }
        if (ready == 0) {
            return {};
        }

        std::vector<std::uint8_t> buffer(maxBytes);
        const auto rc = ::read(fd_, buffer.data(), buffer.size());
        if (rc < 0) {
            if (errno == EINTR && std::chrono::steady_clock::now() < deadline) {
                continue;
            }
            if (errno == EINTR) {
                return {};
            }
            const auto error = errno;
            close();
            throw std::system_error(error, std::generic_category(), "serial read failed");
        }
        if (rc == 0) {
            close();
            throw std::runtime_error("serial connection closed");
        }
        buffer.resize(static_cast<std::size_t>(rc));
        traceBytes(false, buffer.data(), buffer.size());
        return buffer;
    }
}

speed_t PosixSerialPort::baudToTermios(int baudRate) {
    switch (baudRate) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default:
            throw std::invalid_argument("unsupported baud rate");
    }
}

}  // namespace edge_gateway

#endif
