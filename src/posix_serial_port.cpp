#ifndef _WIN32

#include "edge_gateway/posix_serial_port.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
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

    fd_ = ::open(options_.device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd_ < 0) {
        throw std::runtime_error("failed to open serial device: " + options_.device);
    }

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        const auto err = errno;
        close();
        throw std::system_error(err, std::generic_category(), "tcgetattr failed");
    }

    cfmakeraw(&tty);
    const auto baud = baudToTermios(options_.baudRate);
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
    if (options_.parity == "E") {
        tty.c_cflag |= PARENB;
    } else if (options_.parity == "O") {
        tty.c_cflag |= PARENB | PARODD;
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        const auto err = errno;
        close();
        throw std::system_error(err, std::generic_category(), "tcsetattr failed");
    }
}

void PosixSerialPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool PosixSerialPort::isOpen() const {
    return fd_ >= 0;
}

void PosixSerialPort::write(const std::vector<std::uint8_t>& bytes) {
    if (fd_ < 0) {
        throw std::runtime_error("serial port is not open");
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto rc = ::write(fd_, bytes.data() + written, bytes.size() - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "serial write failed");
        }
        if (rc == 0) {
            throw std::runtime_error("serial write returned zero bytes");
        }
        written += static_cast<std::size_t>(rc);
    }
    if (tcdrain(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(), "serial drain failed");
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
            throw std::system_error(errno, std::generic_category(), "select failed");
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
            throw std::system_error(errno, std::generic_category(), "serial read failed");
        }
        buffer.resize(static_cast<std::size_t>(rc));
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
