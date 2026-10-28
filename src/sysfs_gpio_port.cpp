#include "edge_gateway/sysfs_gpio_port.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

// export is retried once inside a call; the gpio directory is created by the export write itself,
// the short poll only covers a slow kernel/udev. A call never blocks for more than ~200 ms.
constexpr int kExportAttempts = 2;
constexpr int kExportPollMs = 10;
constexpr int kExportPollCount = 10;
// A gpio that failed to export is retried at most this often, so a wrong gpio number does not
// stall the collection cycle of the other inputs.
constexpr std::int64_t kExportRetryBackoffMs = 5000;

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool pathExists(const std::string& path) {
#ifndef _WIN32
    return ::access(path.c_str(), F_OK) == 0;
#else
    std::ifstream probe(path.c_str());
    return probe.good();
#endif
}

std::string errnoText(int error) {
    return std::strerror(error);
}

}  // namespace

SysfsGpioPort::SysfsGpioPort(std::string basePath) : basePath_(std::move(basePath)) {
    if (basePath_.empty()) {
        basePath_ = "/sys/class/gpio";
    }
}

SysfsGpioPort::~SysfsGpioPort() {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : valueReadFds_) {
        if (entry.second >= 0) ::close(entry.second);
    }
    for (const auto& entry : valueWriteFds_) {
        if (entry.second >= 0) ::close(entry.second);
    }
#endif
}

void SysfsGpioPort::exportGpio(int gpio) {
    if (gpio < 0) {
        throw std::invalid_argument("gpio must be non-negative");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    exportGpioLocked(gpio);
}

void SysfsGpioPort::exportGpioLocked(int gpio) {
    if (exported_.find(gpio) != exported_.end()) {
        return;
    }
    const auto directionPath = gpioPath(gpio, "direction");
    const auto retryAt = exportRetryAtMs_.find(gpio);
    if (retryAt != exportRetryAtMs_.end() && steadyMs() < retryAt->second && !pathExists(directionPath)) {
        throw std::runtime_error("gpio " + std::to_string(gpio) + " not exported (export failed, retrying in " +
                                 std::to_string(retryAt->second - steadyMs()) + " ms)");
    }
    std::string lastError = "gpio directory did not appear";
    for (int attempt = 0; attempt < kExportAttempts; ++attempt) {
        if (!pathExists(directionPath)) {
            try {
                writeFile(basePath_ + "/export", std::to_string(gpio));
            } catch (const std::exception& ex) {
                // EBUSY when exported concurrently; the directory check below decides.
                lastError = ex.what();
            }
        }
        for (int poll = 0; poll <= kExportPollCount; ++poll) {
            if (pathExists(directionPath)) {
                exported_.insert(gpio);
                exportRetryAtMs_.erase(gpio);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kExportPollMs));
        }
    }
    // Not remembered as exported: the next access after the backoff exports again.
    exportRetryAtMs_[gpio] = steadyMs() + kExportRetryBackoffMs;
    throw std::runtime_error("failed to export gpio " + std::to_string(gpio) + ": " + lastError);
}

bool SysfsGpioPort::currentlyOutputLocked(int gpio) {
    const auto cached = directions_.find(gpio);
    if (cached != directions_.end() && cached->second == "out") {
        return true;
    }
    const auto actual = readFile(gpioPath(gpio, "direction"));
    if (actual.compare(0, 3, "out") == 0) {
        directions_[gpio] = "out";
        return true;
    }
    return false;
}

void SysfsGpioPort::setDirection(int gpio, const std::string& direction) {
    if (direction != "in" && direction != "out") {
        throw std::invalid_argument("gpio direction must be in or out");
    }
    exportGpio(gpio);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = directions_.find(gpio);
    if (current != directions_.end() && current->second == direction) {
        return;
    }
    writeFile(gpioPath(gpio, "direction"), direction);
    directions_[gpio] = direction;
}

void SysfsGpioPort::setOutputDirection(int gpio, bool initialHigh) {
    exportGpio(gpio);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Writing "out" drives the pin low before the value is written, which pulses an active-low
        // relay. An existing output keeps its direction; otherwise use the kernel's glitch-free
        // "high"/"low" direction values.
        if (!currentlyOutputLocked(gpio)) {
            writeFile(gpioPath(gpio, "direction"), initialHigh ? "high" : "low");
            directions_[gpio] = "out";
        }
    }
    writeValue(gpio, initialHigh);
}

bool SysfsGpioPort::ensureOutput(int gpio, bool initialHigh) {
    exportGpio(gpio);
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentlyOutputLocked(gpio)) {
        return false;
    }
    writeFile(gpioPath(gpio, "direction"), initialHigh ? "high" : "low");
    directions_[gpio] = "out";
    return true;
}

bool SysfsGpioPort::readValue(int gpio) {
    exportGpio(gpio);
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = valueReadFds_.find(gpio);
    if (it == valueReadFds_.end()) {
        const auto fd = ::open(gpioPath(gpio, "value").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("failed to open gpio value for read: " + gpioPath(gpio, "value"));
        }
        it = valueReadFds_.emplace(gpio, fd).first;
    }
    char value = '0';
    if (::lseek(it->second, 0, SEEK_SET) < 0 || ::read(it->second, &value, 1) != 1) {
        ::close(it->second);
        valueReadFds_.erase(it);
        exported_.erase(gpio);
        throw std::runtime_error("failed to read gpio value: " + gpioPath(gpio, "value"));
    }
    return value == '1';
#else
    const auto value = readFile(gpioPath(gpio, "value"));
    return !value.empty() && value[0] == '1';
#endif
}

void SysfsGpioPort::writeValue(int gpio, bool high) {
    exportGpio(gpio);
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = valueWriteFds_.find(gpio);
    if (it == valueWriteFds_.end()) {
        const auto fd = ::open(gpioPath(gpio, "value").c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("failed to open gpio value for write: " + gpioPath(gpio, "value"));
        }
        it = valueWriteFds_.emplace(gpio, fd).first;
    }
    const char value = high ? '1' : '0';
    if (::lseek(it->second, 0, SEEK_SET) < 0 || ::write(it->second, &value, 1) != 1) {
        ::close(it->second);
        valueWriteFds_.erase(it);
        exported_.erase(gpio);
        throw std::runtime_error("failed to write gpio value: " + gpioPath(gpio, "value"));
    }
#else
    writeFile(gpioPath(gpio, "value"), high ? "1" : "0");
#endif
}

std::string SysfsGpioPort::gpioPath(int gpio, const std::string& file) const {
    return basePath_ + "/gpio" + std::to_string(gpio) + "/" + file;
}

void SysfsGpioPort::writeFile(const std::string& path, const std::string& value) const {
#ifndef _WIN32
    // sysfs reports a rejected value from write() or close(); both are checked.
    const auto fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("failed to open gpio file for write: " + path + ": " + errnoText(errno));
    }
    const auto written = ::write(fd, value.data(), value.size());
    if (written != static_cast<ssize_t>(value.size())) {
        const auto error = written < 0 ? errno : EIO;
        ::close(fd);
        throw std::runtime_error("failed to write gpio file: " + path + ": " + errnoText(error));
    }
    if (::close(fd) != 0) {
        throw std::runtime_error("failed to write gpio file: " + path + ": " + errnoText(errno));
    }
#else
    std::ofstream output(path.c_str());
    if (!output.is_open()) {
        throw std::runtime_error("failed to open gpio file for write: " + path);
    }
    output << value;
    output.flush();
    if (!output.good()) {
        throw std::runtime_error("failed to write gpio file: " + path);
    }
#endif
}

std::string SysfsGpioPort::readFile(const std::string& path) const {
    std::ifstream input(path.c_str());
    if (!input.is_open()) {
        throw std::runtime_error("failed to open gpio file for read: " + path);
    }
    // sysfs attributes are short; never read more than a line.
    char buffer[64] = {};
    input.read(buffer, sizeof(buffer) - 1);
    return std::string(buffer, static_cast<std::size_t>(input.gcount()));
}

}  // namespace edge_gateway
